
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
#include "fileio.h"

#ifdef HAVE_LINUX_EVENTFD
#include <sys/eventfd.h>
#endif
#include <sys/select.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <iostream>

using namespace std;


namespace acng
{

conn::conn(cmstring& sClientHost) : m_sClientHost(sClientHost)
{
	if(!inBuf.setsize(32*1024))
		throw std::bad_alloc();
};

conn::~conn() {
	LOGSTART("con::~con (Destroying connection...)");

	m_jobs2send.clear();

	if(m_pDlClient) m_pDlClient->Shutdown();

	writeAnotherLogRecord(sEmptyString, sEmptyString);

#warning shutdown, fix
//	if(m_pDlClient)
//		m_pDlClient->becomeRonin();

	log::flush();

	if(m_event)
		event_free(m_event);
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
#warning error, shit, mach daraus ein item mit error-text und con-close
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


short conn::socketAction(int fd, short what)
{
	LOGSTART(__FUNCTION__);

	// can always get new jobs
	short ret = EV_READ;

	if (!inBuf.empty() || (what & EV_READ))
	{
		inBuf.move();

		if (what & EV_READ)
		{
			int n = inBuf.sysread(fd);
			ldbg("got data: " << n <<", inbuf size: "<< inBuf.size());
			if (n <= 0) // error, incoming junk overflow or closed connection
			{
				if (n != -EAGAIN)
				{
					ldbg("client closed connection");
					return 0;
				}
			}
		}

		// split new data into requests
		while (inBuf.size() > 0)
		{
			try
			{
				header h;
				int nHeadBytes = h.Load(inBuf.rptr(), inBuf.size());
				ldbg("header parsed how? " << nHeadBytes);

				// Either not enough data received, or buffer full; make space and retry later when buffer was shrinked
				if (nHeadBytes == 0)
					return EV_READ;

				if (nHeadBytes < 0)
				{
					ldbg("Bad request: " << inBuf.rptr() );
					return 0;
				}

				// also must be identified before
				if (h.type == header::POST)
				{
					if (cfg::forwardsoap && !m_sClientHost.empty())
					{
						if (RawPassThrough::CheckListbugs (h))
						{
							tSplitWalk iter(&h.frontLine);
							if (iter.Next() && iter.Next())
							{
#warning ein redirect-response hier einbauen, auch constraint dass es erster request ist
								RawPassThrough::RedirectBto2https(fd, iter);
								return EV_WRITE;
							}
						}
						else
						{
							ldbg("not bugs.d.o: " << inBuf.rptr());
						}
						// disconnect anyhow
						return 0;
					} ldbg("not allowed POST request: " << inBuf.rptr());
					return 0;
				}

				// handle a request to play as raw tunnel
				// currently only supported as the first request
				// if later other handling is needed,
				// then might need special job item which handles the switch later
				if (h.type == header::CONNECT)
				{
					inBuf.drop(nHeadBytes);

					// connect as non-first request...
					// :-( support we not right now
					if(!m_jobs2send.empty())
					{
						return 0;
					}

					tSplitWalk iter(&h.frontLine);
					if (iter.Next() && iter.Next())
					{
						cmstring tgt(iter);
						if (rex::Match(tgt, rex::PASSTHROUGH))
						{
#warning implement pt-mode here, using j_sendbuf as outgoing buffer
							//Switch2RawPassThrough();
							return EV_READ | EV_WRITE;
							// RawPassThrough::PassThrough(inBuf, m_confd, tgt);
						}
						else
						{
							m_jobs2send.emplace_back(h, *this);
							m_jobs2send.back().PrepareErrorResponse()
									<< "HTTP/1." <<
									( m_jobs2send.back().m_bIsHttp11 ? "1" : "0")
									<< " 403 CONNECT denied (ask the admin to allow HTTPS tunnels)\r\n\r\n";
							return EV_WRITE;
						}
					}
					// anything else?
					return 0;
				}

				if (m_sClientHost.empty()) // may come from UDS wrapper... MUST identify itself first
				{
					inBuf.drop(nHeadBytes);

					if (h.h[header::XORIG] && *(h.h[header::XORIG]))
					{
						m_sClientHost = h.h[header::XORIG];
						continue; // OK, next header?
					}
					else
						return 0;
				}

				ldbg("Parsed REQUEST:" << h.frontLine); ldbg("Rest: " << (inBuf.size()-nHeadBytes));
				m_jobs2send.emplace_back(h, *this);
				m_jobs2send.back().PrepareDownload(inBuf.rptr());
				inBuf.drop(nHeadBytes);

				ret |= EV_WRITE;

			} catch (bad_alloc&)
			{
				return 0;
			}
		}
	}

	if(what&EV_WRITE)
	{
		if(m_jobs2send.empty())
			return 0; // who requested write, wtf?

		auto& j = m_jobs2send.back();
		j.SendData(fd);
		ldbg("Job step result: " << int(j.m_stateExternal));
		switch (j.m_stateExternal)
		{
		case job::XSTATE_DISCON:
			return 0;
		case job::XSTATE_CAN_SEND:
			return EV_READ | EV_WRITE;
		case job::XSTATE_WAIT_DL:
			return EV_READ; // blocked by download, write-check event will be re-added by downloader when it got data
		case job::XSTATE_FINISHED:
			m_jobs2send.pop_front();
			ClearSharedMembers();
			ldbg("Remaining jobs to send: " << m_jobs2send.size())
			;
		}
	}

	return ret;
}

#if 0
void conn::WorkLoop()
{
	while(true)
	{
		bool checkDlFd = false;
		job *pjSender = m_jobs2send.empty() ? nullptr :m_jobs2send.front();

		// usually can always send unless socket is busy or the download is ours and we wait for it
		if (pjSender && pjSender->m_stateExternal == job::XSTATE_CAN_SEND)
			FD_SET(m_confd, &wfds);

		if(pjSender)
		{
			if (pjSender->m_eDlType == job::DLSTATE_OUR && m_pDlClient && !dlPaused)
			{
				// just do it here
				if (m_lastDlState.flags & dlcon::tWorkState::needConnect)
					m_lastDlState = m_pDlClient->Work(dlcon::canConnect);
				checkDlFd = (m_lastDlState.flags
						& (dlcon::tWorkState::needRecv | dlcon::tWorkState::needSend));
				if (checkDlFd)
				{
					if (m_lastDlState.fd > maxfd)
						maxfd = m_lastDlState.fd;
					if (m_lastDlState.flags & dlcon::tWorkState::needSend)
						FD_SET(m_lastDlState.fd, &wfds);
					if (m_lastDlState.flags & dlcon::tWorkState::needRecv)
						FD_SET(m_lastDlState.fd, &rfds);
				}
			}
			if (pjSender->m_eDlType == job::DLSTATE_OTHER)
			{
				int fd = getEventReadFd();
				if (fd > maxfd)
					maxfd = fd;
				FD_SET(fd, &rfds);
			}
		}

		ldbg("select con");

		int ready = select(maxfd+1, &rfds, &wfds, nullptr, GetNetworkTimeout());

		if(ready == 0)
		{
			USRDBG("Timeout occurred, apt client disappeared silently?");
			if(checkDlFd)
			{
				m_lastDlState = m_pDlClient->Work(dlcon::ioretGotTimeout);
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
				m_pDlClient->Work(dlcon::ioretGotError);

			return; // FIXME: good error message?
		}

		ldbg("select con back");

		// first let the downloader work so that job handler is likely to get its data ASAP
		unsigned checkDlResult = 0;
		if(checkDlFd)
		{
			if((m_lastDlState.flags & dlcon::tWorkState::needSend) && FD_ISSET(m_lastDlState.fd, &wfds))
				checkDlResult |= dlcon::ioretCanSend;
			if((m_lastDlState.flags & dlcon::tWorkState::needRecv) && FD_ISSET(m_lastDlState.fd, &rfds))
				checkDlResult |= dlcon::ioretCanRecv;
			if(checkDlResult)
				m_lastDlState = m_pDlClient->Work(checkDlResult);
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


		if(inBuf.freecapa()==0)
			return; // cannot happen unless being attacked

		if (pjSender)
		{
			bool trySend = (pjSender->m_stateExternal == job::XSTATE_CAN_SEND
					&& FD_ISSET(m_confd, &wfds));
#ifdef HAVE_LINUX_EVENTFD
			int fd = m_wakeventfd;
#else
			int fd = m_wakepipe[0];
#endif
			if (pjSender->m_stateExternal == job::XSTATE_WAIT_DL)
			{
				if (pjSender->m_eDlType == job::DLSTATE_OUR && checkDlResult)
					trySend = true;
				else if (pjSender->m_eDlType == job::DLSTATE_OTHER && FD_ISSET(fd, &rfds))
				{
					trySend = true; // and also needs to clean the event fd
					eventFetch();
				}
			}

			if (trySend)
			{

			}
		}
	}
}
#endif

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
#warning wie leben einhauhen? das ding hüpft von fd zu fd
//		m_lastDlState = m_pDlClient->Work(dlcon::freshStart);
//		return ! (dlcon::tWorkState::fatalError & m_lastDlState.flags);

		return true;
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

bool conn::Subscribe4updates(tFileItemPtr fitem)
{
	if(!setupEventFd())
		return false;
	fitem->subscribe(getEventWriteFd());
	return true;
}

void conn::UnsubscribeFromUpdates(tFileItemPtr fitem)
{
	fitem->unsubscribe(getEventWriteFd());
}
}

