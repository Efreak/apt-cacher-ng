
//#define LOCAL_DEBUG
#include "debug.h"

#include "meta.h"
#include "conn.h"
#include "job.h"
#include "header.h"
#include "dlcon.h"
#include "acbuf.h"
#include "tcpconnect.h"
#include "cleaner.h"

#include <sys/select.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <iostream>

using namespace std;


namespace acng
{

conn::conn(int fdId, const char *c) :
			m_confd(fdId),
			m_pDlClient(nullptr),
			m_pTmpHead(nullptr)
{
	if(c) // if nullptr, pick up later when sent by the wrapper
		m_sClientHost=c;

	LOGSTART2("con::con", "fd: " << fdId << ", clienthost: " << c);

#ifdef DEBUG
	m_nProcessedJobs=0;
#endif

};

conn::~conn() {
	LOGSTART("con::~con (Destroying connection...)");
	termsocket(m_confd);
	writeAnotherLogRecord(sEmptyString, sEmptyString);
	for(auto& j: m_jobs2send) delete j;
	delete m_pDlClient;
	delete m_pTmpHead;
	log::flush();
#warning recycle update fds
}

namespace RawPassThrough
{

#define POSTMARK "POST http://bugs.debian.org:80/"

inline static bool CheckListbugs(const header &ph)
{
	return (0 == ph.frontLine.compare(0, _countof(POSTMARK) - 1, POSTMARK));
}
inline static void RedirectBto2https(int fdClient, cmstring& uri)
{
	tSS clientBufOut;
	clientBufOut << "HTTP/1.1 302 Redirect\r\nLocation: " << "https://bugs.debian.org:443/";
	constexpr auto offset = _countof(POSTMARK) - 6;
	clientBufOut.append(uri.c_str() + offset, uri.size() - offset);
	clientBufOut << "\r\nConnection: close\r\n\r\n";
	clientBufOut.send(fdClient);
	// XXX: there is a minor risk of confusing the client if the POST body is bigger than the
	// incoming buffer (probably 64k). But OTOH we shutdown the connection properly, so a not
	// fully stupid client should cope with that. Maybe this should be investigate better.
	return;
}
void PassThrough(acbuf &clientBufIn, int fdClient, cmstring& uri)
{
	tDlStreamHandle m_spOutCon;

	string sErr;
	tSS clientBufOut;
	clientBufOut.setsize(32 * 1024); // out to be enough for any BTS response

	// arbitrary target/port, client cares about SSL handshake and other stuff
	tHttpUrl url;
	if (!url.SetHttpUrl(uri))
		return;
	auto proxy = cfg::GetProxyInfo();
	if (!proxy)
	{
		direct_connect:
		m_spOutCon = g_tcp_con_factory.CreateConnected(url.sHost, url.GetPort(), sErr, 0, 0,
				false, cfg::nettimeout, true);
	}
	else
	{
		// switch to HTTPS tunnel in order to get a direct connection through the proxy
		m_spOutCon = g_tcp_con_factory.CreateConnected(proxy->sHost, proxy->GetPort(),
				sErr, 0, 0, false, cfg::optproxytimeout > 0 ?
						cfg::optproxytimeout : cfg::nettimeout,
						true);

		if (m_spOutCon)
		{
			if (!m_spOutCon->StartTunnel(tHttpUrl(url.sHost, url.GetPort(),
					true), sErr, & proxy->sUserPass, false))
			{
				m_spOutCon.reset();
			}
		}
		else if(cfg::optproxytimeout > 0) // ok... try without
		{
			cfg::MarkProxyFailure();
			goto direct_connect;
		}
	}

	if (m_spOutCon)
		clientBufOut << "HTTP/1.0 200 Connection established\r\n\r\n";
	else
	{
		clientBufOut << "HTTP/1.0 502 CONNECT error: " << sErr << "\r\n\r\n";
		clientBufOut.send(fdClient);
		return;
	}

	if (!m_spOutCon)
		return;

	// for convenience
	int ofd = m_spOutCon->GetFD();
	acbuf &serverBufOut = clientBufIn, &serverBufIn = clientBufOut;

	int maxfd = 1 + std::max(fdClient, ofd);

	while (true)
	{
		fd_set rfds, wfds;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);

		// can send to client?
		if (clientBufOut.size() > 0)
			FD_SET(fdClient, &wfds);

		// can receive from client?
		if (clientBufIn.freecapa() > 0)
			FD_SET(fdClient, &rfds);

		if (serverBufOut.size() > 0)
			FD_SET(ofd, &wfds);

		if (serverBufIn.freecapa() > 0)
			FD_SET(ofd, &rfds);

		int nReady = select(maxfd, &rfds, &wfds, nullptr, nullptr);
		if (nReady < 0)
			return;

		if (FD_ISSET(ofd, &wfds))
			if (serverBufOut.syswrite(ofd) < 0)
				return;

		if (FD_ISSET(fdClient, &wfds))
			if (clientBufOut.syswrite(fdClient) < 0)
				return;

		if (FD_ISSET(ofd, &rfds))
			if (serverBufIn.sysread(ofd) <= 0)
				return;

		if (FD_ISSET(fdClient, &rfds))
			if (clientBufIn.sysread(fdClient) <= 0)
				return;
	}
	return;
}
}

void conn::WorkLoop() {

	LOGSTART("con::WorkLoop");

	signal(SIGPIPE, SIG_IGN);

	acbuf inBuf;
	inBuf.setsize(32*1024);

	bool bWaitDl = false;

	int maxfd=m_confd;
	while(true) {
		fd_set rfds, wfds;
		FD_ZERO(&wfds);
		FD_ZERO(&rfds);

		FD_SET(m_confd, &rfds);
		if(inBuf.freecapa()==0)
			return; // shouldn't even get here

		job *pjSender(nullptr);

		if ( !m_jobs2send.empty() && !bWaitDl)
		{
			pjSender=m_jobs2send.front();
			FD_SET(m_confd, &wfds);
		}

		bool checkDlFd = false;
		if(m_pDlClient && !dlPaused)
		{
			// just do it here
			if(m_lastDlState.flags & dlcon::tWorkState::needConnect)
				m_lastDlState = m_pDlClient->WorkLoop(dlcon::canConnect);
			checkDlFd = (m_lastDlState.flags & (dlcon::tWorkState::needRecv|dlcon::tWorkState::needSend));
			if(checkDlFd)
			{
				if(m_lastDlState.fd > maxfd)
					maxfd = m_lastDlState.fd;
				if(m_lastDlState.flags & dlcon::tWorkState::needSend)
					FD_SET(m_lastDlState.fd, &wfds);
				if(m_lastDlState.flags & dlcon::tWorkState::needRecv)
					FD_SET(m_lastDlState.fd, &rfds);
			}
		}

		ldbg("select con");

		struct timeval tv;
		tv.tv_sec = cfg::nettimeout;
		tv.tv_usec = 23;
		int ready = select(maxfd+1, &rfds, &wfds, nullptr, &tv);

		if(ready == 0)
		{
			USRDBG("Timeout occurred, apt client disappeared silently?");
			if(checkDlFd)
			{
				m_lastDlState = m_pDlClient->WorkLoop(dlcon::ioretGotTimeout);
				continue;
			}
			return;
		}
		else if (ready<0)
		{
			if (EINTR == errno)
				continue;

			ldbg("select error in con, errno: " << errno);

			if(checkDlFd)
				m_pDlClient->WorkLoop(dlcon::ioretGotError);

			return; // FIXME: good error message?
		}

		ldbg("select con back");

		// first let the downloader work so that job handler is likely to get its data ASAP
		if(checkDlFd)
		{
			unsigned hint = 0;
			if((m_lastDlState.flags & dlcon::tWorkState::needSend) && FD_ISSET(m_lastDlState.fd, &wfds))
				hint |= dlcon::ioretCanSend;
			if((m_lastDlState.flags & dlcon::tWorkState::needRecv) && FD_ISSET(m_lastDlState.fd, &rfds))
				hint |= dlcon::ioretCanRecv;
			if(hint)
			{
				bWaitDl = false;
				m_lastDlState = m_pDlClient->WorkLoop(hint);
			}
		}

		if(FD_ISSET(m_confd, &rfds)) {
			int n=inBuf.sysread(m_confd);
			ldbg("got data: " << n <<", inbuf size: "<< inBuf.size());
			if(n<=0) // error, incoming junk overflow or closed connection
			{
				if(n==-EAGAIN)
					continue;
				else
				{
					ldbg("client closed connection");
					Shutdown();

					return;
				}
			}
		}

		// split new data into requests
		while(inBuf.size()>0) {
			try
			{
				if(!m_pTmpHead)
					m_pTmpHead = new header();
				if(!m_pTmpHead)
					return; // no resources? whatever

				m_pTmpHead->clear();
				int nHeadBytes=m_pTmpHead->Load(inBuf.rptr(), inBuf.size());
				ldbg("header parsed how? " << nHeadBytes);
				if(nHeadBytes == 0)
				{ // Either not enough data received, or buffer full; make space and retry
					inBuf.move();
					break;
				}
				if(nHeadBytes < 0)
				{
					ldbg("Bad request: " << inBuf.rptr() );
					return;
				}

				// also must be identified before
				if (m_pTmpHead->type == header::POST)
				{
					if (cfg::forwardsoap && !m_sClientHost.empty())
					{
						if (RawPassThrough::CheckListbugs(*m_pTmpHead))
						{
							tSplitWalk iter(&m_pTmpHead->frontLine);
							if(iter.Next() && iter.Next())
								RawPassThrough::RedirectBto2https(m_confd, iter);
						}
						else
						{
							ldbg("not bugs.d.o: " << inBuf.rptr());
						}
						// disconnect anyhow
						return;
					}
					ldbg("not allowed POST request: " << inBuf.rptr());
					return;
				}

				if(m_pTmpHead->type == header::CONNECT)
				{

					inBuf.drop(nHeadBytes);

					tSplitWalk iter(&m_pTmpHead->frontLine);
					if(iter.Next() && iter.Next())
					{
						cmstring tgt(iter);
						if(rex::Match(tgt, rex::PASSTHROUGH))
							RawPassThrough::PassThrough(inBuf, m_confd, tgt);
						else
						{
							tSS response;
							response << "HTTP/1.0 403 CONNECT denied (ask the admin to allow HTTPS tunnels)\r\n\r\n";
							while(!response.empty())
								response.syswrite(m_confd);
						}
					}
					return;
				}

				if (m_sClientHost.empty()) // may come from wrapper... MUST identify itself
				{

					inBuf.drop(nHeadBytes);

					if(m_pTmpHead->h[header::XORIG] && *(m_pTmpHead->h[header::XORIG]))
					{
						m_sClientHost=m_pTmpHead->h[header::XORIG];
						continue; // OK
					}
					else
						return;
				}

				ldbg("Parsed REQUEST:" << m_pTmpHead->frontLine);
				ldbg("Rest: " << (inBuf.size()-nHeadBytes));

				{
					job * j = new job(m_pTmpHead, *this);
#warning clear scratch data here?
					j->PrepareDownload(inBuf.rptr());
					inBuf.drop(nHeadBytes);

					m_jobs2send.emplace_back(j);
#ifdef DEBUG
					m_nProcessedJobs++;
#endif
				}

				m_pTmpHead=nullptr; // owned by job now
			}
			catch(bad_alloc&)
			{
				return;
			}
		}

		if(inBuf.freecapa()==0)
			return; // cannot happen unless being attacked

		if (pjSender && (pjSender->m_stateExternal == job::XSTATE_CAN_SEND
				|| FD_ISSET(m_confd, &wfds)))
		{
			bWaitDl = false;
			pjSender->SendData(m_confd);
			ldbg("Job step result: " << pjSender->m_stateExternal);
			switch(pjSender->m_stateExternal)
			{
			case job::XSTATE_DISCON: return;
			case job::XSTATE_CAN_SEND: continue; // come back ASAP
			case job::XSTATE_WAIT_DL: bWaitDl = true; break;
			case job::XSTATE_FINISHED:
				m_jobs2send.pop_front();
				delete pjSender;
				pjSender = nullptr;
				ldbg("Remaining jobs to send: " << m_jobs2send.size());
			}
		}
	}
}

bool conn::SetupDownloader(const char *pszOrigin)
{
	if (m_pDlClient)
		return true;

	try
	{
		if(cfg::exporigin)
		{
			string sXff;
			if(pszOrigin)
			{
				sXff = *pszOrigin;
				sXff += ", ";
			}
			sXff+=m_sClientHost;
			m_pDlClient=new dlcon(&sXff);
		}
		else
			m_pDlClient=new dlcon;

		if(!m_pDlClient)
			return false;

		m_lastDlState = m_pDlClient->WorkLoop(dlcon::freshStart);
		return ! (dlcon::tWorkState::fatalError & m_lastDlState.flags);

	}
	catch(bad_alloc&)
	{
		return false;
	}
}

void conn::LogDataCounts(cmstring & sFile, const char *xff, off_t nNewIn,
		off_t nNewOut, bool bAsError)
{
	string sClient;
	if (!cfg::logxff || !xff) // not to be logged or not available
		sClient=m_sClientHost;
	else if (xff)
	{
		sClient=xff;
		trimString(sClient);
		string::size_type pos = sClient.find_last_of(SPACECHARS);
		if (pos!=stmiss)
			sClient.erase(0, pos+1);
	}
	if(sFile != logFile || sClient != logClient)
		writeAnotherLogRecord(sFile, sClient);
	fileTransferIn += nNewIn;
	fileTransferOut += nNewOut;
	if(bAsError) m_bLogAsError = true;
}

// sends the stats to logging and replaces file/client identities with the new context
void conn::writeAnotherLogRecord(const mstring &pNewFile, const mstring &pNewClient)
{
		log::transfer(fileTransferIn, fileTransferOut, logClient, logFile, m_bLogAsError);
		fileTransferIn = fileTransferOut = 0;
		m_bLogAsError = false;
		logFile = pNewFile;
		logClient = pNewClient;
}

void conn::Shutdown()
{
	for(auto& j: m_jobs2send) delete j;
	m_jobs2send.clear();
	if(m_pDlClient) m_pDlClient->Shutdown();
#warning close wake pipe or donate that descriptor(s) to some cache
}

void conn::Subscribe4updates(tFileItemPtr)
{
#warning implement
}

void conn::UnsubscribeFromUpdates(tFileItemPtr)
{
#warning implement
}
}

