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
#include "dnsiter.h"
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

ACNG_API std::atomic_uint dl_con_factory::g_nconns(0);
ACNG_API dl_con_factory g_tcp_con_factory;

tcpconnect::tcpconnect(cfg::tRepoData::IHookHandler *pObserver) : m_pStateObserver(pObserver)
{
	if(cfg::maxdlspeed != cfg::RESERVED_DEFVAL)
		dl_con_factory::g_nconns.fetch_add(1);
	if(pObserver)
		pObserver->OnAccess();
}

tcpconnect::~tcpconnect()
{
	LOGSTART("tcpconnect::~tcpconnect, terminating outgoing connection class");
	Disconnect();
	if(cfg::maxdlspeed != cfg::RESERVED_DEFVAL)
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

	Disconnect();
	auto time_start(GetTime());

	enum ePhase {
		NO_ALTERNATIVES,
		NOT_YET,
		PICK_ADDR,
		ADDR_PICKED,
		SELECT_CONN, // shall do select on connection
		HANDLE_ERROR, // transient error state, to be continued into some recovery or ERROR_STOP; expects a useful errno value!
		ERROR_STOP // basically a non-recoverable error state
	};
	struct tConData {
		ePhase state;
		int fd;
		time_t tmexp;
		const evutil_addrinfo *dns;
		~tConData() { checkforceclose(fd); }
		//! prepare and start connection
		//! @return true if connection happened even here, false otherwise (state is then adjusted for further processing)
		bool init_con(time_t time_exp)
		{
			checkforceclose(fd);
			tmexp = time_exp;
			fd = ::socket(dns->ai_family, dns->ai_socktype, dns->ai_protocol);
			if(fd == -1)
			{
				state = HANDLE_ERROR;
				return false;
			}
			set_sock_flags(fd);
#if DEBUG
			log::err(string("Connecting: ") + formatIpPort(dns));
#endif
			while (true)
			{
				auto res = connect(fd, dns->ai_addr, dns->ai_addrlen);
				if (res != -1)
					return true;
				if (errno == EINTR)
					continue;
				if (errno == EINPROGRESS)
				{
					errno = 0;
					state = SELECT_CONN;
				}
				else
					// interpret that errno
					state = HANDLE_ERROR;
				return false;

			}
		}
	};
	auto iter = tAlternatingDnsIterator(dns->getTcpAddrInfo());
	tConData prim {ADDR_PICKED, -1, time_start + timeout, iter.next() };
	tConData alt {NO_ALTERNATIVES, -1, time_start + cfg::fasttimeout, nullptr };
	CTimeVal tv;
	// pickup the first and/or probably the best errno code which can be reported to user
	int error_prim = 0;

	auto retGood = [&](int& fd) { std::swap(fd, m_conFd); return true; };
	auto retError = [&](const std::string &errStr) { sErrorMsg = errStr; return false; };
	auto withErrnoError = [&]() { return retError(tErrnoFmter("500 Connection failure: "));	};
	auto withThisErrno = [&withErrnoError](int myErr) { errno = myErr; return withErrnoError(); };

	// ok, initial condition, one target should be always there, iterator would also hop to the next fallback if allowed
	if(!prim.dns)
		return withThisErrno(EAFNOSUPPORT);
	if (cfg::fasttimeout > 0)
	{
		alt.dns = iter.next();
		alt.state = alt.dns ? NOT_YET : NO_ALTERNATIVES;
	}

	for(auto op_max=0; op_max < 30000; ++op_max) // fail-safe switch, in case of any mistake here
	{
		LOG("state a: " << prim.state << ", state b: " << alt.state );
		switch(prim.state)
		{
		case PICK_ADDR:
		case NO_ALTERNATIVES:
		case NOT_YET:
			// XXX: not reachable
			break;
		case ADDR_PICKED:
			if(prim.init_con(time_start + cfg::nettimeout))
				return retGood(prim.fd);
			OPTSET(error_prim, errno);
			__just_fall_through;
		case SELECT_CONN:
		{
			if(GetTime() >= prim.tmexp)
			{
				OPTSET(error_prim, ELVIS(errno, ETIMEDOUT));
				prim.state = HANDLE_ERROR;
				continue;
			}
			break;
		}
		case HANDLE_ERROR:
		{
			// error on primary, what now? prefer the first seen error code or remember errno
			OPTSET(error_prim, ELVIS(errno, EINVAL));
			// can work around?
			switch(alt.state)
			{
			case NO_ALTERNATIVES:
			case ERROR_STOP:
				return withThisErrno(error_prim);
			case NOT_YET:
				prim.state = ERROR_STOP;
				// push it sooner
				alt.tmexp -= cfg::fasttimeout;
				break;
			default:
				// some intermediate state there? Let it continue
				prim.state = ERROR_STOP;
				break;
			}
			break;
		}
		case ERROR_STOP:
			checkforceclose(prim.fd);
			break;
		default: // this should be unreachable
			return retError("500 Internal error at " STRINGIFY(__LINE__));
		}

		switch(alt.state)
		{
		case NO_ALTERNATIVES:
			break;
		case NOT_YET:
			if (GetTime() < alt.tmexp)
				break;
			// first DNS info was preselected before
			ASSERT(alt.dns);
			alt.state = ADDR_PICKED;
			__just_fall_through;
		case ADDR_PICKED:
		{
			if(alt.init_con(GetTime() + cfg::fasttimeout))
				return retGood(alt.fd);
			continue;
		}
		case PICK_ADDR:
		{
			alt.dns = iter.next();
			alt.state = alt.dns ? ADDR_PICKED : ERROR_STOP;
			continue;
		}
		case SELECT_CONN:
		{
			auto now(GetTime());
			if(now >= prim.tmexp)
			{
				alt.state = ERROR_STOP;
				continue;
			}
			if(now >= alt.tmexp)
			{
				alt.state = HANDLE_ERROR;
				continue;
			}
			break;
		}
		case HANDLE_ERROR:
		{
			alt.state = PICK_ADDR;
			continue;
		}
		case ERROR_STOP:
		{
			// came here because:
			// - no more alternative DNS info
			// - fatal socket/connect errors
			// - exceeded maximal network timeout
			if(prim.state == ERROR_STOP)
			{
				// reconsider final error state there
				prim.state = HANDLE_ERROR;
				continue;
			}
			break;
		}
		default: // this should be unreachable
			return retError("500 Internal error at " STRINGIFY(__LINE__));
		}

		select_set_t selset;
		auto time_inter = prim.tmexp;
		if(prim.state == SELECT_CONN)
			selset.add(prim.fd);
		if(alt.state == SELECT_CONN)
			selset.add(alt.fd);
		if(alt.state == NOT_YET || alt.state == SELECT_CONN)
		{
			if(alt.tmexp < time_inter)
				time_inter = alt.tmexp;
		}

		auto res = select(selset.nfds(), nullptr, &selset.fds, nullptr, tv.Remaining(time_inter));
		if (res < 0)
		{
			if (EINTR != errno)
				return withErrnoError();
		}
		else if (res > 0)
		{
			for(auto p: {&alt, &prim})
			{
				// Socket selected for writing.
				int err;
				socklen_t optlen = sizeof(err);
				if(p->state == SELECT_CONN && selset.is_set(p->fd)
				&& getsockopt(p->fd, SOL_SOCKET, SO_ERROR, (void*) &err, &optlen) == 0)
				{
					if(err)
					{
						OPTSET(error_prim, err);
						prim.state = HANDLE_ERROR;
					}
					else
						return retGood(p->fd);
				}
			}
		}
		else
		{
			// Timeout.
			errno = ETIMEDOUT;
			continue;
		}

	}
	return withThisErrno(ELVIS(error_prim, EINVAL));
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

ACNG_API void CloseAllCachedConnections()
{
	lockguard g(spareConPoolMx);
	spareConPool.clear();
}

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
			spareConPool.emplace(make_tuple(host, handle->GetPort()
					SSL_OPT_ARG(handle->m_bio) ), make_pair(handle, now));
#ifndef MINIBUILD
			cleaner::GetInstance().ScheduleFor(now + TIME_SOCKET_EXPIRE_CLOSE, cleaner::TYPE_EXCONNS);
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
	int r=select(nMaxFd + 1, &rfds, nullptr, nullptr, CTimeVal().For(0, 1));
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
	mstring ebuf;

	auto withSslError = [&sErr](const char *perr)
					{
		sErr="500 SSL error: ";
		sErr+=(perr?perr:"Generic SSL failure");
		return false;
					};
	auto withLastSslError = [&withSslError]()
						{
		return withSslError(ERR_reason_error_string(ERR_get_error()));
						};
	auto withRetCode = [&withSslError, &ssl](int hret)
				{
		return withSslError(ERR_reason_error_string(SSL_get_error(ssl, hret)));
				};

	// cleaned up in the destructor on EOL
	if(!m_ctx)
	{
		m_ctx = SSL_CTX_new(SSLv23_client_method());
		if (!m_ctx) return withLastSslError();

		SSL_CTX_load_verify_locations(m_ctx,
				cfg::cafile.empty() ? nullptr : cfg::cafile.c_str(),
			cfg::capath.empty() ? nullptr : cfg::capath.c_str());
	}

	ssl = SSL_new(m_ctx);
	if (!m_ctx) return withLastSslError();

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

 	auto hret=SSL_set_fd(ssl, m_conFd);
 	if(hret != 1) return withRetCode(hret);

 	while(true)
 	{
 		hret=SSL_connect(ssl);
 		if(hret == 1 )
 			break;
 		if(hret == 0)
 			return withRetCode(hret);

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
 			return withRetCode(hret);
 		}
		int nReady=select(m_conFd+1, &rfds, &wfds, nullptr, CTimeVal().ForNetTimeout());
		if(!nReady) return withSslError("Socket timeout");
		if (nReady<0)
		{
#ifndef MINIBUILD
			ebuf=tErrnoFmter("Socket error");
			return withSslError(ebuf.c_str());
#else
			return withSslError("Socket error");
#endif
		}
 	}
 	if(m_bio) BIO_free_all(m_bio);
 	m_bio = BIO_new(BIO_f_ssl());
 	if(!m_bio) return withSslError("IO initialization error");
 	// not sure we need it but maybe the handshake can access this data
 	BIO_set_conn_hostname(m_bio, sHostname.c_str());
 	BIO_set_conn_port(m_bio, sPort.c_str());

 	BIO_set_ssl(m_bio, ssl, BIO_NOCLOSE);

 	BIO_set_nbio(m_bio, 1);
	set_nb(m_conFd);

	if(!cfg::nsafriendly)
	{
		X509* server_cert = nullptr;
		hret=SSL_get_verify_result(ssl);
		if( hret != X509_V_OK)
			return withSslError(X509_verify_cert_error_string(hret));
		server_cert = SSL_get_peer_certificate(ssl);
		if(server_cert)
		{
			// XXX: maybe extract the real name to a buffer and report it additionally?
			// X509_NAME_oneline(X509_get_subject_name (server_cert), cert_str, sizeof (cert_str));
			X509_free(server_cert);
		}
		else // The handshake was successful although the server did not provide a certificate
			return withSslError("Incompatible remote certificate");
	}
	return true;
}

//! Global initialization helper (might be non-reentrant)
void ACNG_API globalSslInit()
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

std::string formatIpPort(const evutil_addrinfo *p)
{
	char buf[300], pbuf[30];
	getnameinfo(p->ai_addr, p->ai_addrlen, buf, sizeof(buf), pbuf, sizeof(pbuf),
			NI_NUMERICHOST | NI_NUMERICSERV);
	return string(p->ai_family == PF_INET6 ? "[" : "") +
			buf +
			(p->ai_family == PF_INET6 ? "]" : "") +
			":" + pbuf;
}



}
