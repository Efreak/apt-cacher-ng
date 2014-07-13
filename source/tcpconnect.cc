/*
 * tcpconnect.cpp
 *
 *  Created on: 27.02.2010
 *      Author: ed
 */

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

using namespace std;

//#warning FIXME, hack
//#define NOCONCACHE

#ifdef DEBUG
volatile int nConCount(0), nDisconCount(0), nReuseCount(0);
#endif

#ifdef HAVE_SSL
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

std::atomic_uint tcpconnect::g_nconns(0);

tcpconnect::tcpconnect()
{
	if(acfg::maxdlspeed != RESERVED_DEFVAL)
		g_nconns.fetch_add(1);
}

tcpconnect::~tcpconnect()
{
	LOGSTART("tcpconnect::~tcpconnect, terminating outgoing connection class");
	Disconnect();
	if(acfg::maxdlspeed != RESERVED_DEFVAL)
		g_nconns.fetch_add(-1);
#ifdef HAVE_SSL
	if(m_ctx)
		SSL_CTX_free(m_ctx);
	m_ctx=0;
#endif
}


/*! \brief Helper to flush data stream contents reliable and close the connection then
 * DUDES, who write TCP implementations... why can this just not be done easy and reliable? Why do we need hacks like termsocket?
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

static int connect_timeout(int sockfd, const struct sockaddr *addr, socklen_t addrlen, time_t timeout)
{
	long stflags;
	struct timeval tv;
	fd_set wfds;
	int res;

	tv.tv_sec = timeout;
	tv.tv_usec = 0;

	if ((stflags = fcntl(sockfd, F_GETFL, NULL)) < 0)
		return -1;

	// Set to non-blocking mode.
	if (fcntl(sockfd, F_SETFL, stflags|O_NONBLOCK) < 0)
		return -1;

	res = connect(sockfd, addr, addrlen);
	if (res < 0) {
		if (EINPROGRESS == errno)
		{
			for (;;) {
				// Wait for connection.
				FD_ZERO(&wfds);
				FD_SET(sockfd, &wfds);
				res = select(sockfd+1, NULL, &wfds, NULL, &tv);
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

	// Set back to original mode, which may or may not have been blocking.
	if (fcntl(sockfd, F_SETFL, stflags) < 0)
		return -1;

	return 0;
}

inline bool tcpconnect::Connect(string & sErrorMsg, int timeout)
{
	LOGSTART2("tcpconnect::_Connect", "hostname: " << m_sHostName);

	CAddrInfo::SPtr dns = CAddrInfo::CachedResolve(m_sHostName, m_sPort, sErrorMsg);

	if(!dns)
	{
		USRDBG(sErrorMsg);
		return false; // sErrorMsg got the info already, no other chance to fix it
	}

	::signal(SIGPIPE, SIG_IGN);

	// always consider first family, afterwards stop when no more specified
	for (UINT i=0; i< _countof(acfg::conprotos) && (0==i || acfg::conprotos[i]!=PF_UNSPEC); ++i)
	{
		for (struct addrinfo *pInfo = dns->m_addrInfo; pInfo; pInfo = pInfo->ai_next)
		{
			if (acfg::conprotos[i] != PF_UNSPEC && acfg::conprotos[i] != pInfo->ai_family)
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

			if (::connect_timeout(m_conFd, pInfo->ai_addr, pInfo->ai_addrlen, timeout) < 0)
			{
				if(errno==ETIMEDOUT)
					sErrorMsg="Connection timeout";
				continue;
			}
#ifdef DEBUG
			__sync_fetch_and_add(&nConCount, 1);
#endif
			ldbg("connect() ok");
			set_nb(m_conFd);
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
	if(m_conFd >=0)
		__sync_fetch_and_add(&nDisconCount, 1);
#endif

#ifdef HAVE_SSL
	if(m_bio)
		BIO_free_all(m_bio), m_bio=NULL;
#endif

	if(m_conFd>=0 && m_pConnStateObserver)
		m_pConnStateObserver->JobRelease();

	m_pConnStateObserver=NULL;
	m_lastFile.reset();

	termsocket_quick(m_conFd);
}

using namespace std;
struct tHostHint // could derive from pair but prefer to save some bytes with references
{
	cmstring pHost, pPort;
#ifdef HAVE_SSL
	bool bSsled;
	inline tHostHint(cmstring &h, cmstring &p, bool bSsl) : pHost(h), pPort(p), bSsled(bSsl) {};
	bool operator<(const tHostHint &b) const
	{
		int rel=pHost.compare(b.pHost);
		if(rel)
			return rel < 0;
		if(pPort != b.pPort)
			return pPort < b.pPort;
		return bSsled < b.bSsled;
	}
#else
	inline tHostHint(cmstring &h, cmstring &p) : pHost(h), pPort(p) {};
		bool operator<(const tHostHint &b) const
		{
			int rel=pHost.compare(b.pHost);
			if(rel)
				return rel < 0;
			return pPort < b.pPort;
		}
#endif

};
class : public lockable, public multimap<tHostHint, std::pair<tTcpHandlePtr, time_t> >
{} spareConPool;

tTcpHandlePtr tcpconnect::CreateConnected(cmstring &sHostname, cmstring &sPort,
		mstring &sErrOut, bool *pbSecondHand, acfg::tRepoData::IHookHandler *pStateTracker
		,bool bSsl, int timeout)
{
	LOGSTART2s("tcpconnect::CreateConnected", "hostname: " << sHostname << ", port: " << sPort
			<< (bSsl?" with ssl":" , no ssl"));

	tTcpHandlePtr p;
#ifndef HAVE_SSL
	if(bSsl)
	{
		aclog::err("E_NOTIMPLEMENTED: SSL");
		return p;
	}
#endif

	bool bReused=false;
	tHostHint key(sHostname, sPort
#ifdef HAVE_SSL
			, bSsl
#endif
	);

#ifdef NOCONCACHE
	p.reset(new tcpconnect);
	if(!p || !p->Connect(sHostname, sPort, sErrOut) || p->GetFD()<0) // failed or worthless
		p.reset();
#else
	{ // mutex context
		lockguard __g(spareConPool);
		auto it=spareConPool.find(key);
		if(spareConPool.end() == it)
		{
			p.reset(new tcpconnect);
			if(p)
			{
				p->m_sHostName=sHostname;
				p->m_sPort=sPort;
			}

			if(!p || !p->Connect(sErrOut, timeout) || p->GetFD()<0) // failed or worthless
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
		else
		{
			p=it->second.first;
			spareConPool.erase(it);
			bReused = true;
			ldbg("got connection " << p.get() << " from the idle pool");
#ifdef DEBUG
			__sync_fetch_and_add(&nReuseCount, 1);
#endif
		}
	}
#endif

	if(p && pStateTracker)
	{
		p->m_pConnStateObserver = pStateTracker;
		pStateTracker->JobConnect();
	}

	if(pbSecondHand)
		*pbSecondHand = bReused;

	return p;
}

void tcpconnect::RecycleIdleConnection(tTcpHandlePtr & handle)
{
	if(!handle)
		return;

	LOGSTART2s("tcpconnect::RecycleIdleConnection", handle->m_sHostName);

	if(handle->m_pConnStateObserver)
	{
		handle->m_pConnStateObserver->JobRelease();
		handle->m_pConnStateObserver = NULL;
	}

	if(! acfg::persistoutgoing)
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

#ifndef NOCONCACHE
	time_t now=GetTime();
	lockguard __g(spareConPool);
	ldbg("caching connection " << handle.get());

	// a DOS?
	if(spareConPool.size()<50)
	{
		spareConPool.insert(make_pair(tHostHint(handle->GetHostname(), handle->GetPort()
#ifdef HAVE_SSL
				,handle->m_bio
#endif
		),make_pair(handle, now)));

#ifndef MINIBUILD
		g_victor.ScheduleFor(now+TIME_SOCKET_EXPIRE_CLOSE, cleaner::TYPE_EXCONNS);
#endif

	}
#endif

	handle.reset();
}

time_t tcpconnect::BackgroundCleanup()
{
	lockguard __g(spareConPool);
	time_t now=GetTime();

	fd_set rfds;
	FD_ZERO(&rfds);
	int nMaxFd=0;

	// either drop the old ones, or stuff them into a quick select call to find the good sockets
	for (auto it = spareConPool.begin(); it != spareConPool.end();)
	{
		if (now >= (it->second.second + TIME_SOCKET_EXPIRE_CLOSE))
			spareConPool.erase(it++);
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
	int r=select(nMaxFd + 1, &rfds, NULL, NULL, &tv);
	// on error, also do nothing, or stop when r fds are processed
	for (auto it = spareConPool.begin(); r>0 && it != spareConPool.end(); r--)
	{
		if(FD_ISSET(it->second.first->GetFD(), &rfds))
			spareConPool.erase(it++);
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

void tcpconnect::dump_status()
{
	lockguard __g(spareConPool);
	tSS msg;
	msg << "TCP connection cache:\n";
	for (auto& x : spareConPool)
	{
		if(! x.second.first)
		{
			msg << "[BAD HANDLE] recycle at " << x.second.second << "\n";
			continue;
		}

		msg << x.second.first->m_conFd << ": for "
				<< x.first.pHost << ":" << x.first.pPort
				<< ", recycled at " << x.second.second << "\n";
	}
#ifdef DEBUG
	msg << "dbg counts, con: " << __sync_fetch_and_add(&nConCount, 0)
			<< " , discon: " << __sync_fetch_and_add(&nDisconCount, 0)
			<< " , reuse: " << __sync_fetch_and_add(&nReuseCount, 0) << "\n";
#endif

	aclog::err(msg);
}
#ifdef HAVE_SSL
bool tcpconnect::SSLinit(mstring &sErr, cmstring &sHostname, cmstring &sPort)
{
	SSL * ssl(NULL);
	int hret(0);
	LPCSTR perr(0);

	// cleaned up in the destructor on EOL
	if(!m_ctx)
	{
		m_ctx = SSL_CTX_new(SSLv23_client_method());
		if (!m_ctx)
			goto ssl_init_fail;

		SSL_CTX_load_verify_locations(m_ctx, acfg::cafile.empty() ? NULL : acfg::cafile.c_str(),
			acfg::capath.empty() ? NULL : acfg::capath.c_str());

	}

	ssl = SSL_new(m_ctx);
	if(!ssl)
		goto ssl_init_fail;

	// mark it connected and prepare for non-blocking mode
 	SSL_set_connect_state(ssl);
 	SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY
 			| SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
 			| SSL_MODE_ENABLE_PARTIAL_WRITE);

 	// make it blocking for sure otherwise SSL_connect would become messy
	set_block(m_conFd);

 	if((hret=SSL_set_fd(ssl, m_conFd)) != 1)
 		goto ssl_init_fail_retcode;
	if((hret=SSL_connect(ssl)) != 1)
		goto ssl_init_fail_retcode;

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

	hret=SSL_get_verify_result(ssl);
	if( hret != X509_V_OK)
	{
		perr=X509_verify_cert_error_string(hret);
		goto ssl_init_fail;
	}

	return true;

	ssl_init_fail_retcode:

	perr=ERR_reason_error_string(SSL_get_error(ssl, hret));

	ssl_init_fail:

	if(!perr)
		perr=ERR_reason_error_string(ERR_get_error());
	sErr="SSL error: ";
	sErr+=(perr?perr:"Generic SSL failure");
	return false;
}

#endif
