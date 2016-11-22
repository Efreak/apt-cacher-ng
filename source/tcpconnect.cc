/*
 * tcpconnect.cpp
 *
 *  Created on: 27.02.2010
 *      Author: ed
 */

#include <sys/select.h>

#define LOCAL_DEBUG
#include "debug.h"

#include "meta.h"
#include "tcpconnect.h"

#include "acfg.h"
#include "caddrinfo.h"
#include <signal.h>
#include "fileio.h"
#include "fileitem.h"
#include "cleaner.h"
#include <tuple>

using namespace std;

//#warning FIXME, hack
//#define NOCONCACHE

#ifdef DEBUG
#include <atomic>
atomic_int nConCount(0), nDisconCount(0), nReuseCount(0);
#endif

#ifdef HAVE_SSL
#include <openssl/evp.h>
#include "openssl/bio.h"
#include "openssl/ssl.h"
#include "openssl/err.h"
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#endif

namespace acng
{

std::atomic_uint dl_con_factory::g_nconns(0);
dl_con_factory g_tcp_con_factory;

tcpconnect::tcpconnect(cfg::tRepoData::IHookHandler *pObserver) : m_pStateObserver(pObserver)
{
	if(cfg::maxdlspeed != RESERVED_DEFVAL)
		dl_con_factory::g_nconns.fetch_add(1);
	if(pObserver)
		pObserver->OnAccess();
}

tcpconnect::~tcpconnect()
{
	LOGSTART("tcpconnect::~tcpconnect, terminating outgoing connection class");
	Disconnect();
	if(cfg::maxdlspeed != RESERVED_DEFVAL)
		dl_con_factory::g_nconns.fetch_add(-1);
#ifdef HAVE_SSL
	if(m_ctx)
	{
		SSL_CTX_free(m_ctx);
		m_ctx=0;
	}
#endif
	if(m_pStateObserver)
	{
		m_pStateObserver->OnRelease();
		m_pStateObserver=nullptr;

	}
}

/*! \brief Helper to flush data stream contents reliable and close the connection then
 * DUDES, who write TCP implementations... why can this just not be done easy and reliable? Why do we need hacks like the method below?
 For details, see: http://blog.netherlabs.nl/articles/2009/01/18/the-ultimate-so_linger-page-or-why-is-my-tcp-not-reliable
 *
 */
void termsocket(int fd)
{
	LOGSTART2s("::termsocket", fd);
	if (fd < 0)
		return;

	fcntl(fd, F_SETFL, ~O_NONBLOCK & fcntl(fd, F_GETFL));
	::shutdown(fd, SHUT_WR);
	char buf[40];
	LOG("waiting for peer to react");
	while(true)
	{
		int r=recv(fd, buf, 40, MSG_WAITALL);
		if(0 == r)
			break;
		if(r < 0)
		{
			if(errno == EINTR)
				continue;
			break; // XXX error case, actually
		}
	}

	while (0 != ::close(fd))
	{
		if (errno != EINTR)
			break;
	};
}

static int connect_timeout(int sockfd, const struct sockaddr *addr, socklen_t addrlen, time_t timeout, bool bAssumeNonBlock)
{
	long stflags;
	struct timeval tv;
	fd_set wfds;
	int res;

	tv.tv_sec = timeout;
	tv.tv_usec = 0;

	if(!bAssumeNonBlock)
	{
		if ((stflags = fcntl(sockfd, F_GETFL, nullptr)) < 0)
			return -1;

		// Set to non-blocking mode.
		if (fcntl(sockfd, F_SETFL, stflags | O_NONBLOCK) < 0)
			return -1;
	}
	res = connect(sockfd, addr, addrlen);
	if (res < 0) {
		if (EINPROGRESS == errno)
		{
			for (;;) {
				// Wait for connection.
				FD_ZERO(&wfds);
				FD_SET(sockfd, &wfds);
				res = select(sockfd+1, nullptr, &wfds, nullptr, &tv);
				if (res < 0)
				{
					if (EINTR != errno)
						return -1;
				}
				else if (res > 0)
				{
					// Socket selected for writing.
					int err;
					socklen_t optlen = sizeof(err);

					if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (void *)&err, &optlen) < 0)
						return -1;

					if (err)
					{
						errno = err;
						return -1;
					}

					break;
				} else {
					// Timeout.
					errno = ETIMEDOUT;
					return -1;
				}
			}
		} else {
			return -1;
		}
	}

	if(!bAssumeNonBlock && fcntl(sockfd, F_SETFL, stflags) < 0) // Set back to original mode
		return -1;

	return 0;
}

inline bool tcpconnect::_Connect(string & sErrorMsg, int timeout)
{
	LOGSTART2("tcpconnect::_Connect", "hostname: " << m_sHostName);

	auto dns = CAddrInfo::CachedResolve(m_sHostName, m_sPort, sErrorMsg);

	if(!dns)
	{
		USRDBG(sErrorMsg);
		return false; // sErrorMsg got the info already, no other chance to fix it
	}

	::signal(SIGPIPE, SIG_IGN);

	// always consider first family, afterwards stop when no more specified
	for (unsigned i=0; i< _countof(cfg::conprotos) && (0==i || cfg::conprotos[i]!=PF_UNSPEC); ++i)
	{
		for (auto pInfo = dns->m_addrInfo; pInfo; pInfo = pInfo->ai_next)
		{
			if (cfg::conprotos[i] != PF_UNSPEC && cfg::conprotos[i] != pInfo->ai_family)
				continue;

			ldbg("Creating socket for " << m_sHostName);

			if (pInfo->ai_socktype != SOCK_STREAM || pInfo->ai_protocol != IPPROTO_TCP)
				continue;

			Disconnect();

			m_conFd = ::socket(pInfo->ai_family, pInfo->ai_socktype, pInfo->ai_protocol);
			if (m_conFd < 0)
				continue;

#ifndef NO_TCP_TUNNING
			{
				int yes(1);
				::setsockopt(m_conFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
				::setsockopt(m_conFd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
			}
#endif
			set_nb(m_conFd);
			if (acng::connect_timeout(m_conFd, pInfo->ai_addr, pInfo->ai_addrlen, timeout, true) < 0)
			{
				if(errno==ETIMEDOUT)
					sErrorMsg="Connection timeout";
#ifndef MINIBUILD
				USRDBG(tErrnoFmter("Outgoing connection for ") << m_sHostName << ", Port: " << m_sPort );
#endif
				continue;
			}
#ifdef DEBUG
			nConCount.fetch_add(1);
#endif
			ldbg("connect() ok");

			return true;
		}
	}

#ifdef MINIBUILD
	sErrorMsg = "500 Connection failure";
#else
	// format the last available error message for the user
	sErrorMsg=tErrnoFmter("500 Connection failure: ");
#endif
	ldbg("Force reconnect, con. failure");
	Disconnect();
	return false;
}

void tcpconnect::Disconnect()
{
	LOGSTART("tcpconnect::_Disconnect");

#ifdef DEBUG
	nDisconCount.fetch_add(m_conFd >=0);
#endif

#ifdef HAVE_SSL
	if(m_bio)
		BIO_free_all(m_bio), m_bio=nullptr;
#endif

	m_lastFile.reset();

	termsocket_quick(m_conFd);
}
acmutex spareConPoolMx;
multimap<tuple<string,string SSL_OPT_ARG(bool) >,
		std::pair<tDlStreamHandle, time_t> > spareConPool;

tDlStreamHandle dl_con_factory::CreateConnected(cmstring &sHostname, cmstring &sPort,
		mstring &sErrOut, bool *pbSecondHand, cfg::tRepoData::IHookHandler *pStateTracker
		,bool bSsl, int timeout, bool nocache)
{
	LOGSTART2s("tcpconnect::CreateConnected", "hostname: " << sHostname << ", port: " << sPort
			<< (bSsl?" with ssl":" , no ssl"));

	tDlStreamHandle p;
#ifndef HAVE_SSL
	if(bSsl)
	{
		log::err("E_NOTIMPLEMENTED: SSL");
		return p;
	}
#endif

	bool bReused=false;
	auto key = make_tuple(sHostname, sPort SSL_OPT_ARG(bSsl) );

#ifdef NOCONCACHE
	p.reset(new tcpconnect(pStateTracker));
	if(p)
	{
		if(!p->_Connect(sHostname, sPort, sErrOut) || p->GetFD()<0) // failed or worthless
			p.reset();
	}
#else
	if(!nocache)
	{
		// mutex context
		lockguard __g(spareConPoolMx);
		auto it=spareConPool.find(key);
		if(spareConPool.end() != it)
		{
			p=it->second.first;
			spareConPool.erase(it);
			bReused = true;
			ldbg("got connection " << p.get() << " from the idle pool");

			// it was reset in connection recycling, restart now
			if(pStateTracker)
			{
				p->m_pStateObserver = pStateTracker;
				pStateTracker->OnAccess();
			}
#ifdef DEBUG
			nReuseCount.fetch_add(1);
#endif
		}
	}
#endif

	if(!p)
	{
		p.reset(new tcpconnect(pStateTracker));
		if(p)
		{
			p->m_sHostName=sHostname;
			p->m_sPort=sPort;
		}

		if(!p || !p->_Connect(sErrOut, timeout) || p->GetFD()<0) // failed or worthless
			p.reset();
#ifdef HAVE_SSL
		else if(bSsl)
		{
			if(!p->SSLinit(sErrOut, sHostname, sPort))
			{
				p.reset();
				LOG("ssl init error");
			}
		}
#endif
	}

	if(pbSecondHand)
		*pbSecondHand = bReused;

	return p;
}

void dl_con_factory::RecycleIdleConnection(tDlStreamHandle & handle)
{
	if(!handle)
		return;

	LOGSTART2s("tcpconnect::RecycleIdleConnection", handle->m_sHostName);

	if(handle->m_pStateObserver)
	{
		handle->m_pStateObserver->OnRelease();
		handle->m_pStateObserver = nullptr;
	}

	if(! cfg::persistoutgoing)
	{
		ldbg("not caching outgoing connections, drop " << handle.get());
		handle.reset();
		return;
	}

#ifdef DEBUG
	if(check_read_state(handle->GetFD()))
	{
		acbuf checker;
		checker.setsize(300000);
		checker.sysread(handle->GetFD());

	}
#endif

	auto& host = handle->GetHostname();
	if (!host.empty())
	{
#ifndef NOCONCACHE
		time_t now = GetTime();
		lockguard __g(spareConPoolMx);
		ldbg("caching connection " << handle.get());

		// a DOS?
		if (spareConPool.size() < 50)
		{
			EMPLACE_PAIR_COMPAT(spareConPool, make_tuple(host, handle->GetPort()
					SSL_OPT_ARG(handle->m_bio) ), make_pair(handle, now));
#ifndef MINIBUILD
			g_victor.ScheduleFor(now + TIME_SOCKET_EXPIRE_CLOSE, cleaner::TYPE_EXCONNS);
#endif
		}
#endif
	}

	handle.reset();
}

time_t dl_con_factory::BackgroundCleanup()
{
	lockguard __g(spareConPoolMx);
	time_t now=GetTime();

	fd_set rfds;
	FD_ZERO(&rfds);
	int nMaxFd=0;

	// either drop the old ones, or stuff them into a quick select call to find the good sockets
	for (auto it = spareConPool.begin(); it != spareConPool.end();)
	{
		if (now >= (it->second.second + TIME_SOCKET_EXPIRE_CLOSE))
			it = spareConPool.erase(it);
		else
		{
			int fd = it->second.first->GetFD();
			FD_SET(fd, &rfds);
			nMaxFd = max(nMaxFd, fd);
			++it;
		}
	}
	// if they have to send something, that must the be the CLOSE signal
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 1;
	int r=select(nMaxFd + 1, &rfds, nullptr, nullptr, &tv);
	// on error, also do nothing, or stop when r fds are processed
	for (auto it = spareConPool.begin(); r>0 && it != spareConPool.end(); r--)
	{
		if(FD_ISSET(it->second.first->GetFD(), &rfds))
			it = spareConPool.erase(it);
		else
			++it;
	}

	return spareConPool.empty() ? END_OF_TIME : GetTime()+TIME_SOCKET_EXPIRE_CLOSE/4+1;
}

void tcpconnect::KillLastFile()
{
#ifndef MINIBUILD
	tFileItemPtr p = m_lastFile.lock();
	if (!p)
		return;
	p->SetupClean(true);
#endif
}

void dl_con_factory::dump_status()
{
	lockguard __g(spareConPoolMx);
	tSS msg;
	msg << "TCP connection cache:\n";
	for (const auto& x : spareConPool)
	{
		if(! x.second.first)
		{
			msg << "[BAD HANDLE] recycle at " << x.second.second << "\n";
			continue;
		}

		msg << x.second.first->m_conFd << ": for "
				<< get<0>(x.first) << ":" << get<1>(x.first)
				<< ", recycled at " << x.second.second
				<< "\n";
	}
#ifdef DEBUG
	msg << "dbg counts, con: " << nConCount.load()
			<< " , discon: " << nDisconCount.load()
			<< " , reuse: " << nReuseCount.load() << "\n";
#endif

	log::err(msg);
}
#ifdef HAVE_SSL
bool tcpconnect::SSLinit(mstring &sErr, cmstring &sHostname, cmstring &sPort)
{
	SSL * ssl(nullptr);
	int hret(0);
	LPCSTR perr(0);
	mstring ebuf;

	// cleaned up in the destructor on EOL
	if(!m_ctx)
	{
		m_ctx = SSL_CTX_new(SSLv23_client_method());
		if (!m_ctx)
			goto ssl_init_fail;

		SSL_CTX_load_verify_locations(m_ctx,
				cfg::cafile.empty() ? nullptr : cfg::cafile.c_str(),
			cfg::capath.empty() ? nullptr : cfg::capath.c_str());
	}

	ssl = SSL_new(m_ctx);
	if(!ssl)
		goto ssl_init_fail;

	// for SNI
	SSL_set_tlsext_host_name(ssl, sHostname.c_str());

	{
		auto param = SSL_get0_param(ssl);
		/* Enable automatic hostname checks */
		X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
		X509_VERIFY_PARAM_set1_host(param, sHostname.c_str(), 0);
		/* Configure a non-zero callback if desired */
		SSL_set_verify(ssl, SSL_VERIFY_PEER, 0);
	}

	// mark it connected and prepare for non-blocking mode
 	SSL_set_connect_state(ssl);
 	SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY
 			| SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
 			| SSL_MODE_ENABLE_PARTIAL_WRITE);

 	if((hret=SSL_set_fd(ssl, m_conFd)) != 1)
 		goto ssl_init_fail_retcode;

 	while(true)
 	{
 		hret=SSL_connect(ssl);
 		if(hret == 1 )
 			break;
 		if(hret == 0)
 			goto ssl_init_fail_retcode;

		fd_set rfds, wfds;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
 		switch(SSL_get_error(ssl, hret))
 		{
 		case SSL_ERROR_WANT_READ:
 			FD_SET(m_conFd, &rfds);
 			break;
 		case SSL_ERROR_WANT_WRITE:
 			FD_SET(m_conFd, &wfds);
 			break;
 		default:
 			goto ssl_init_fail_retcode;
 		}
 		struct timeval tv;
 		tv.tv_sec = cfg::nettimeout;
 		tv.tv_usec = 0;
		int nReady=select(m_conFd+1, &rfds, &wfds, nullptr, &tv);
		if(!nReady)
		{
			perr="Socket timeout";
			goto ssl_init_fail;
		}
		if (nReady<0)
		{
#ifndef MINIBUILD
			ebuf=tErrnoFmter("Socket error");
			perr=ebuf.c_str();
#else
			perr="Socket error";
#endif
			goto ssl_init_fail;
		}
 	}

 	m_bio = BIO_new(BIO_f_ssl());
 	if(!m_bio)
 	{
 		perr="IO initialization error";
 		goto ssl_init_fail;
 	}
 	// not sure we need it but maybe the handshake can access this data
 	BIO_set_conn_hostname(m_bio, sHostname.c_str());
 	BIO_set_conn_port(m_bio, sPort.c_str());

 	BIO_set_ssl(m_bio, ssl, BIO_NOCLOSE);

 	BIO_set_nbio(m_bio, 1);
	set_nb(m_conFd);

	if(!cfg::nsafriendly)
	{
		hret=SSL_get_verify_result(ssl);
		if( hret != X509_V_OK)
		{
			perr=X509_verify_cert_error_string(hret);
			goto ssl_init_fail;
		}
	}

	return true;

	ssl_init_fail_retcode:

	perr=ERR_reason_error_string(SSL_get_error(ssl, hret));

	ssl_init_fail:

	if(!perr)
		perr=ERR_reason_error_string(ERR_get_error());
	sErr="500 SSL error: ";
	sErr+=(perr?perr:"Generic SSL failure");
	return false;
}

//! Global initialization helper (might be non-reentrant)
void globalSslInit()
{
	static bool inited=false;
	if(inited)
		return;
	inited = true;
	SSL_load_error_strings();
	ERR_load_BIO_strings();
	ERR_load_crypto_strings();
	ERR_load_SSL_strings();
	OpenSSL_add_all_algorithms();
	SSL_library_init();
}

#endif

bool tcpconnect::StartTunnel(const tHttpUrl& realTarget, mstring& sError,
		cmstring *psAuthorization, bool bDoSSL)
{
	/*
	  CONNECT server.example.com:80 HTTP/1.1
      Host: server.example.com:80
      Proxy-Authorization: basic aGVsbG86d29ybGQ=
	 */
	tSS fmt;
	fmt << "CONNECT " << realTarget.sHost << ":" << realTarget.GetPort()
			<< " HTTP/1.1\r\nHost: " << realTarget.sHost << ":" << realTarget.GetPort()
			<< "\r\n";
	if(psAuthorization && !psAuthorization->empty())
	{
			fmt << "Proxy-Authorization: Basic "
					<< EncodeBase64Auth(*psAuthorization) << "\r\n";
	}
	fmt << "\r\n";

	try
	{
		if (!fmt.send(m_conFd, &sError))
			return false;

		fmt.clear();
		while (true)
		{
			fmt.setsize(4000);
			if (!fmt.recv(m_conFd, &sError))
				return false;
			if(fmt.freecapa()<=0)
			{
				sError = "503 Remote proxy error";
				return false;
			}

			header h;
			auto n = h.Load(fmt.rptr(), fmt.size());
			if(!n)
				continue;

			auto st = h.getStatus();
			if (n <= 0 || st == 404 /* just be sure it doesn't send crap */)
			{
				sError = "503 Tunnel setup failed";
				return false;
			}

			if (st < 200 || st >= 300)
			{
				sError = h.frontLine;
				return false;
			}
			break;
		}

		m_sHostName = realTarget.sHost;
		m_sPort = realTarget.GetPort();
#ifdef HAVE_SSL
		if (bDoSSL && !SSLinit(sError, m_sHostName, m_sPort))
		{
			m_sHostName.clear();
			return false;
		}
#else
		(void) bDoSSL;
#endif
	}
	catch(...)
	{
		return false;
	}
	return true;
}



}
