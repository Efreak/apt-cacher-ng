
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
#include "conserver.h"

#include <sys/select.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <iostream>

using namespace std;

#define SHORT_TIMEOUT 4

namespace acng
{

conn::conn(unique_fd fd, const char *c) :
			m_fd(move(fd)),
			m_confd(m_fd.get())
{
	if(c) // if nullptr, pick up later when sent by the wrapper
		m_sClientHost=c;

	LOGSTART2("con::con", "fd: " << m_confd << ", clienthost: " << c);

#ifdef DEBUG
	m_nProcessedJobs=0;
#endif

};

conn::~conn() {
	LOGSTART("con::~con (Destroying connection...)");

	// our user's connection is released but the downloader task created here may still be serving others
	// tell it to stop when it gets the chance and delete it then

	for (auto jit : m_jobs2send) delete jit;

	writeAnotherLogRecord(sEmptyString, sEmptyString);

	if(m_pDlClient)
		m_pDlClient->SignalStop();
	if(m_dlerthr.joinable())
		m_dlerthr.join();
	log::flush();
	conserver::FinishConnection(m_confd);
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

	// define a much shorter timeout than network timeout in order to be able to disconnect bad clients quickly
	auto client_timeout(GetTime() + cfg::nettimeout);

	int maxfd=m_confd;
	while(!g_global_shutdown && !m_badState) {
		fd_set rfds, wfds;
		FD_ZERO(&wfds);
		FD_ZERO(&rfds);

		FD_SET(m_confd, &rfds);
		if(inBuf.freecapa()==0)
			return; // shouldn't even get here

		job *pjSender(nullptr);
		bool hasMoreJobs = m_jobs2send.size()>1;

		if ( !m_jobs2send.empty())
		{
			pjSender=m_jobs2send.front();
			FD_SET(m_confd, &wfds);
		}

		ldbg("select con");
		int ready = select(maxfd+1, &rfds, &wfds, nullptr, CTimeVal().For(SHORT_TIMEOUT));

		if(g_global_shutdown)
			break;

		if(ready == 0)
		{
			USRDBG("Timeout occurred, apt client disappeared silently?");
			if(GetTime() > client_timeout)
				return; // yeah, time to leave
			continue;
		}
		else if (ready<0)
		{
			if (EINTR == errno)
				continue;

			ldbg("select error in con, errno: " << errno);
			return; // FIXME: good error message?
		}
		else
		{
			// ok, something is still flowing, increase deadline
			client_timeout = GetTime() + cfg::nettimeout;
		}

		ldbg("select con back");

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
					return;
				}
			}
		}

		// split new data into requests
		while(inBuf.size()>0) {
			try
			{
				header h;
				int nHeadBytes=h.Load(inBuf.rptr(), inBuf.size());
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
				if (h.type == header::POST)
				{
					if (cfg::forwardsoap && !m_sClientHost.empty())
					{
						if (RawPassThrough::CheckListbugs(h))
						{
							tSplitWalk iter(&h.frontLine);
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

				if(h.type == header::CONNECT)
				{

					inBuf.drop(nHeadBytes);

					tSplitWalk iter(& h.frontLine);
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

					if(h.h[header::XORIG] && * h.h[header::XORIG])
					{
						m_sClientHost=h.h[header::XORIG];
						continue; // OK
					}
					else
						return;
				}

				ldbg("Parsed REQUEST:" << h.frontLine);
				ldbg("Rest: " << (inBuf.size()-nHeadBytes));

				{
					job * j = new job(std::move(h), this);
					j->PrepareDownload(inBuf.rptr());

					if(m_badState) return;

					inBuf.drop(nHeadBytes);

					m_jobs2send.emplace_back(j);
#ifdef DEBUG
					m_nProcessedJobs++;
#endif
				}
			}
			catch(bad_alloc&)
			{
				return;
			}
		}

		if(inBuf.freecapa()==0)
			return; // cannot happen unless being attacked

		if(FD_ISSET(m_confd, &wfds) && pjSender)
		{
			switch(pjSender->SendData(m_confd, hasMoreJobs))
			{
			case(job::R_DISCON):
				{
					ldbg("Disconnect advise received, stopping connection");
					return;
				}
			case(job::R_DONE):
				{
					m_jobs2send.pop_front();
					delete pjSender;
					pjSender=nullptr;

					ldbg("Remaining jobs to send: " << m_jobs2send.size());
					break;
				}
			case(job::R_AGAIN):
				break;
			default:
				break;
			}
		}
	}
}

bool conn::SetupDownloader(const char *pszOrigin)
{
	if(m_badState)
		return false;

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
			m_pDlClient.reset(new dlcon(false, &sXff));
		}
		else
			m_pDlClient.reset(new dlcon(false));

		if(!m_pDlClient)
			return false;
		auto pin = m_pDlClient;
		m_dlerthr = move(thread([pin](){
			pin->WorkLoop();
		}));
		m_badState = false;
		return true;
	}
	catch(...)
	{
		m_badState = true;
		m_pDlClient.reset();
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

}
