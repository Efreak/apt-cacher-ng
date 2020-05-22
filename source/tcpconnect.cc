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
#include "tcpconfactory.h"
#include "acfg.h"
#include "caddrinfo.h"
#include <signal.h>
#include "fileio.h"
#include "fileitem.h"
#include "cleaner.h"
#include "dnsiter.h"
#include "acngbase.h"
#include <event.h>
#include <thread>

#include "tlsio.h"

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

#define BUG_COUNT 30000

namespace acng
{


tcpconnect::tcpconnect(tSysRes& res, cmstring& sHost, cmstring& sPort, cfg::IHookHandler *pObserver) :
		m_sysres(res), m_sHostName(sHost), m_sPort(sPort),
		m_pStateObserver(pObserver)
{
	if(pObserver)
		pObserver->OnAccess();
	if(m_pCacheCheckEvent)
		event_free(m_pCacheCheckEvent);
}

tcpconnect::~tcpconnect()
{
	LOGSTART("tcpconnect::~tcpconnect, terminating outgoing connection class");

	m_lastFile.reset();
#ifdef HAVE_SSL
	if (m_bio)
		BIO_free(m_bio);
	if (m_ssl)
		SSL_free(m_ssl);
#endif
	termsocket_quick(m_conFd);
	if (m_pStateObserver)
		m_pStateObserver->OnRelease();
}

struct tcpconnect::tConnProgress
{
	tDlStreamHandle han;
	int timeout;
	IDlConFactory::funcRetCreated resRep;
	tConnProgress(tDlStreamHandle h, int t, IDlConFactory::funcRetCreated r, tSysRes& sr)
	: han(move(h)), timeout(t), resRep(r), prim(sr), alt(sr)
	{
	}

	SHARED_PTR<CAddrInfo> dnsres;

	time_t total_timeout = GetTime() + cfg::nettimeout + 1;

	struct tConData
	{
		unique_fd fd;
		tAutoEv ev;
		tSysRes& m_sr;
		const evutil_addrinfo *dns = nullptr;
		explicit tConData(tSysRes& rs):m_sr(rs){}

		enum class eConnRes
		{
			FOUND,
			STARTED,
			ERROR
		};
		void stop()
		{
			ev.reset();
			fd.reset();
		}
		void init_timeout(event_callback_fn cb, void *arg)
		{
			ev.reset(event_new(m_sr.GetEventBase(), -1, EV_TIMEOUT, cb, arg));
			event_add(*ev, CTimeVal().Remaining(cfg::fasttimeout));
		}
		eConnRes init_con(event_callback_fn cb, int tmout,void *arg)
		{
			fd.reset(::socket(dns->ai_family, dns->ai_socktype, dns->ai_protocol));
			if(*fd == -1)
				return eConnRes::ERROR;
			set_connect_sock_flags(fd.get());
#ifdef DEBUG
			log::err(string("Connecting: ") + formatIpPort(dns));
#endif
			while (true)
			{
				auto res = connect(*fd, dns->ai_addr, dns->ai_addrlen);
				if (res != -1)
					return eConnRes::FOUND;
				if (errno == EINTR)
					continue;
				if (errno == EINPROGRESS)
				{
					errno = 0;
					ev.reset(event_new(m_sr.GetEventBase(), *fd, EV_READ|EV_WRITE, cb, arg));
					event_add(*ev, CTimeVal().For(tmout));
					return eConnRes::STARTED;
				}
				return eConnRes::ERROR;
			}
		}
		// return true if ready, false if not (and error code is stored in errno)
		bool check_avail()
		{
			socklen_t optlen = sizeof(errno);
			return (getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, (void*) &errno, &optlen) == 0 && 0 == errno);
		}
	};
	tAlternatingDnsIterator iter;
	tConData prim; // = {ADDR_PICKED, -1, time_start + timeout, iter.next(), tAutoEv() };
	tConData alt; // = {NO_ALTERNATIVES, -1, time_start + cfg::fasttimeout, nullptr, tAutoEv() };

	// pickup the first and/or probably the best errno code which can be reported to user
	int err_code = 0;

	srt retErr(cmstring &sError)
	{
		IDlConFactory::tConRes result { move(han), sError, false };
		auto rfunc = move(resRep);
		rfunc(move(result));
		return srt::a;
	};
	srt retBad(int ec)
	{
		return retErr(tErrnoFmter("500 Connection failure: ", ec));
	}
	srt retGood(int &fd)
	{
		std::swap(fd, han->m_conFd); // @suppress("Invalid arguments")
		return retErr(sEmptyString);
	};
#warning test simulated timeouts, maybe dedicated unit/integration test which suppresses the callback and runs one later
	static void cb_alt(evutil_socket_t, short what, void *arg) { ((tConnProgress*)arg)->on_event_alt(what&EV_TIMEOUT); }
	static void cb_prim(evutil_socket_t, short what, void *arg) { ((tConnProgress*)arg)->on_event_prim(what&EV_TIMEOUT); }

	srt on_event_prim(bool timedout)
	{
		if(timedout)
			return retBad(ETIMEDOUT);
		if(prim.check_avail())
			return retGood(prim.fd.m_p);
		if(alt.ev.get()) // alt.flow still active, will continue there, remember primary error code
		{
			err_code = errno;
			prim.stop();
			return srt::a;
		}
		return retBad(errno);
	}
	srt on_event_alt(bool tmedout)
	{
#warning detect shutdown here?
		if(/*alt.m_sr.in_shutdown || */
				GetTime() > total_timeout)
			return retBad(ETIMEDOUT);
		bool was_poll = alt.fd.get() != -1;
		bool do_check = !tmedout && was_poll;
		bool connected = do_check && alt.check_avail();
		if(connected)
			return retGood(alt.fd.m_p);
		if(!err_code && do_check)
			err_code = errno;
		if(was_poll) // not in the start where it was fetched in the pre-check
			alt.dns = iter.next();
		if(!alt.dns)
		{
			alt.stop();
			// if prim. flow still active then it will continue
			return prim.ev.m_p ? srt::a : retBad(err_code);
		}
		switch(alt.init_con(cb_alt, cfg::fasttimeout, this))
		{
		case tConData::eConnRes::STARTED:
			return srt::a; // will come back
		case tConData::eConnRes::FOUND:
			return retGood(alt.fd.m_p);
		case tConData::eConnRes::ERROR:
			if(!err_code)
				err_code = errno;
			// if prim. flow still active then it will continue
			return prim.ev.m_p ? srt::a : retBad(err_code);
		}
		ASSERT(!"Unreachable");
		return srt::a;
	}
	srt Connect(shared_ptr<CAddrInfo> dns)
	{
		if(!dns)
			return retBad(ENETUNREACH);
		if(!dns->getError().empty())
			return retErr(dns->getError());

		dnsres = move(dns);
		iter = tAlternatingDnsIterator(dnsres->getTcpAddrInfo());

		prim.dns = iter.next();
		if (!prim.dns)
			return retBad(EAFNOSUPPORT);

		switch(prim.init_con(cb_prim, cfg::nettimeout, this))
		{
		case tConData::eConnRes::STARTED:
			if(cfg::fasttimeout > 0 && !!(alt.dns = iter.next()))
				alt.init_timeout(cb_alt, this);
			return srt::a;
		case tConData::eConnRes::FOUND:
			return retGood(prim.fd.m_p);
		case tConData::eConnRes::ERROR:
			err_code = errno;
			prim.stop();
			if(cfg::fasttimeout <= 0 && !(alt.dns = iter.next()))
				return retBad(err_code);
			alt.init_timeout(cb_alt, this);
			return srt::a;
		default:
			return srt::a;
		}
	}
};

void tcpconnect::DoConnect(cmstring& sHost, cmstring& sPort, cfg::IHookHandler *pStateReport,
		int timeout,
		IDlConFactory::funcRetCreated resRep,
		tSysRes& sysres)
{
	try
	{
		auto han = make_unique<tcpconnect>(sysres, sHost, sPort, pStateReport);
		// XXX: unique_ptr with move fails for unknown reason: copy constructor of '' is implicitly deleted because field '' has a deleted copy constructor
		auto flow = make_shared<tConnProgress>(move(han), timeout, move(resRep), sysres);

		auto after_resolve = [flow](shared_ptr<CAddrInfo> dnsresult)
		{
			// all paths must report through connectFlow
			try
			{
				flow->Connect(dnsresult);
			}
			catch(...)
			{
				flow->retBad(ECONNABORTED);
			}
		};
		sysres.dnsbase->Resolve(sHost, sPort, move(after_resolve));
	}
	catch (...)
	{
		if(resRep)
			resRep(MAKE_CON_RES_DUMMY());
	}
}

void tcpconnect::KillLastFile()
{
#ifndef MINIBUILD
	tFileItemPtr p = m_lastFile.lock();
	if (!p)
		return;
	p->SinkDestroy(true, true);
#endif
}


#ifdef HAVE_SSL
#warning this is all crap, convert to SslUpgrade async function
mstring tcpconnect::SSLinit(cmstring &sHostname, cmstring &sPort)
{
	auto errorStatusLine = [](const char *perr)
	{
		return string("500 SSL error: ") +(perr?perr:"Generic SSL failure");
	};
	auto statusFromSslRetCode = [&](int hret)
	{
		return errorStatusLine(ERR_reason_error_string(SSL_get_error(m_ssl, hret)));
	};

	auto ctx = atls::GetContext();
	if(!ctx)
		return atls::GetInitError();
	m_ssl = SSL_new(ctx);
	if (!m_ssl)
		return errorStatusLine(ERR_reason_error_string(ERR_get_error()));

	// for SNI
	SSL_set_tlsext_host_name(m_ssl, sHostname.c_str());

	auto param = SSL_get0_param(m_ssl);
	/* Enable automatic hostname checks */
	X509_VERIFY_PARAM_set_hostflags(param,
			X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
	X509_VERIFY_PARAM_set1_host(param, sHostname.c_str(), 0);
	/* Configure a non-zero callback if desired */
	SSL_set_verify(m_ssl, SSL_VERIFY_PEER, 0);

	// mark it connected and prepare for non-blocking mode
 	SSL_set_connect_state(m_ssl);
 	SSL_set_mode(m_ssl, SSL_MODE_AUTO_RETRY
 			| SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
 			| SSL_MODE_ENABLE_PARTIAL_WRITE);

 	auto hret=SSL_set_fd(m_ssl, m_conFd);
 	if(hret != 1)
 		return statusFromSslRetCode(hret);

 	while(true)
 	{
 		hret=SSL_connect(m_ssl);
 		if(hret == 1 )
 			break;
 		if(hret == 0)
 			return statusFromSslRetCode(hret);

		fd_set rfds, wfds;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
 		switch(SSL_get_error(m_ssl, hret))
 		{
 		case SSL_ERROR_WANT_READ:
 			FD_SET(m_conFd, &rfds);
 			break;
 		case SSL_ERROR_WANT_WRITE:
 			FD_SET(m_conFd, &wfds);
 			break;
 		default:
 			return statusFromSslRetCode(hret);
 		}
		switch(select(m_conFd+1, &rfds, &wfds, nullptr, CTimeVal().ForNetTimeout()))
		{
		case 0: return errorStatusLine("Socket timeout");
		case -1: return tErrnoFmter("915 Socket error");
		default: break;
		}
 	}
 	m_bio = BIO_new(BIO_f_ssl());
 	if(!m_bio)
 		return errorStatusLine("IO initialization error");
 	// not sure we need it but maybe the handshake can access this data
 	BIO_set_conn_hostname(m_bio, sHostname.c_str());
 	BIO_set_conn_port(m_bio, sPort.c_str());
 	BIO_set_ssl(m_bio, m_ssl, BIO_NOCLOSE);

 	BIO_set_nbio(m_bio, 1);

	if(!cfg::nsafriendly)
	{
		X509* server_cert = nullptr;
		hret=SSL_get_verify_result(m_ssl);
		if(hret != X509_V_OK)
			return errorStatusLine(X509_verify_cert_error_string(hret));
		server_cert = SSL_get_peer_certificate(m_ssl);
		if(server_cert)
		{
			// XXX: maybe extract the real name to a buffer and report it additionally?
			// X509_NAME_oneline(X509_get_subject_name (server_cert), cert_str, sizeof (cert_str));
			X509_free(server_cert);
		}
		else // The handshake was successful although the server did not provide a certificate
			return errorStatusLine("Incompatible remote certificate");
	}
	return sEmptyString;
}

#endif

mstring tcpconnect::StartTunnel(const tHttpUrl& realTarget, cmstring *psAuthorization, bool bDoSSL)
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
		if (!fmt.send(m_conFd))
			return "917 Connection reset by peer";

		fmt.clear();
		while (true)
		{
			fmt.setsize(4000);
			if (!fmt.recv(m_conFd))
				return "916 Connection reset by peer";
			if(fmt.freecapa()<=0)
				return "503 Remote proxy error";

			header h;
			auto n = h.Load(fmt.rptr(), fmt.size());
			if(!n)
				continue;

			auto st = h.getStatus();
			if(AC_UNLIKELY(st == 102))
			{
				// just was keep-alive header, continue...
				fmt.drop(n);
				fmt.move();
				continue;
			}
			if (n < 0 || st == 404 /* just be sure it doesn't send crap */)
				return "503 Tunnel setup failed";
			if (st < 200 || st >= 300)
				return h.frontLine;
			break;
		}

		m_sHostName = realTarget.sHost;
		m_sPort = realTarget.GetPort();
#ifdef HAVE_SSL
		if (bDoSSL)
			return SSLinit(m_sHostName, m_sPort);
#else
		(void) bDoSSL;
#endif
	}
	catch(...)
	{
		return "901 Fatal TUNNEL error";
	}
	return sEmptyString;
}


}
