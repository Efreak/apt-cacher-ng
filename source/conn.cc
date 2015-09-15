
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


con::con(int fdId, const char *c) :
	m_confd(fdId),
    m_bStopActivity(false),
    m_dlerthr(0),
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

con::~con() {
	LOGSTART("con::~con (Destroying connection...)");
	termsocket(m_confd);

	// our user's connection is released but the downloader task created here may still be serving others
	// tell it to stop when it gets the chance and delete it then

	std::list<job*>::iterator jit;
	for (jit=m_jobs2send.begin(); jit!=m_jobs2send.end(); jit++)
		delete *jit;

	logstuff.write();
	
    if(m_pDlClient) 
    {
    	m_pDlClient->SignalStop();
    	pthread_join(m_dlerthr, nullptr);
    	
    	delete m_pDlClient;
    	m_pDlClient=nullptr;
    }
    if(m_pTmpHead)
    {
    	delete m_pTmpHead;
    	m_pTmpHead=nullptr;
    }
    aclog::flush();
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
	if (acfg::proxy_info.sHost.empty())
	{
		direct_connect:
		m_spOutCon = g_tcp_con_factory.CreateConnected(url.sHost, url.GetPort(), sErr, 0, 0,
		false, acfg::nettimeout, true);
	}
	else
	{
		// switch to HTTPS tunnel in order to get a direct connection through the proxy
		m_spOutCon = g_tcp_con_factory.CreateConnected(acfg::proxy_info.sHost, acfg::proxy_info.GetPort(),
				sErr, 0, 0, false, acfg::optproxytimeout > 0 ?
						acfg::optproxytimeout : acfg::nettimeout,
						true);

		if (m_spOutCon)
		{
			if (!m_spOutCon->StartTunnel(tHttpUrl(url.sHost, url.GetPort(),
			true), sErr, &acfg::proxy_info.sUserPass, false))
			{
				m_spOutCon.reset();
			}
		}
		else if(acfg::optproxytimeout > 0) // ok... try without
			goto direct_connect;
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

void con::WorkLoop() {

	LOGSTART("con::WorkLoop");
    
	signal(SIGPIPE, SIG_IGN);
	
    acbuf inBuf;
    inBuf.setsize(32*1024);
    
    int maxfd=m_confd;
    while(!m_bStopActivity) {
        fd_set rfds, wfds;
        FD_ZERO(&wfds);
        FD_ZERO(&rfds);
        
        FD_SET(m_confd, &rfds);
        if(inBuf.freecapa()==0)
        	return; // shouldn't even get here
        
        job *pjSender(nullptr);
    
        if ( !m_jobs2send.empty())
		{
			pjSender=m_jobs2send.front();
			FD_SET(m_confd, &wfds);
		}
		
        
        ldbg("select con");

        struct timeval tv;
        tv.tv_sec = acfg::nettimeout;
        tv.tv_usec = 23;
        int ready = select(maxfd+1, &rfds, &wfds, nullptr, &tv);
        
        if(ready == 0)
        {
        	USRDBG("Timeout occurred, apt client disappeared silently?");
        	return;
        }
		else if (ready<0)
		{
			if (EINTR == errno)
				continue;
			
			ldbg("select error in con, errno: " << errno);
			return; // FIXME: good error message?
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
        	MYTRY
        	{
				if(!m_pTmpHead)
					m_pTmpHead = new header();
				if(!m_pTmpHead)
					return; // no resources? whatever
				
				int nHeadBytes=m_pTmpHead->LoadFromBuf(inBuf.rptr(), inBuf.size());
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
					if (acfg::forwardsoap && !m_sClientHost.empty())
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
						if(rechecks::Match(tgt, rechecks::PASSTHROUGH))
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
					job * j = new job(m_pTmpHead, this);
					j->PrepareDownload(inBuf.rptr());
					inBuf.drop(nHeadBytes);

					m_jobs2send.emplace_back(j);
#ifdef DEBUG
					m_nProcessedJobs++;
#endif
				}

				m_pTmpHead=nullptr; // owned by job now
			}
        	MYCATCH(bad_alloc&)
        	{
        		return;
        	}
        }
        
        if(inBuf.freecapa()==0)
        	return; // cannot happen unless being attacked

		if(FD_ISSET(m_confd, &wfds) && pjSender)
		{
			switch(pjSender->SendData(m_confd))
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

void * _StartDownloader(void *pVoidDler)
{
	static_cast<dlcon*>(pVoidDler) -> WorkLoop();
	return nullptr;
}

bool con::SetupDownloader(const char *pszOrigin)
{
	if (m_pDlClient)
		return true;

	MYTRY
	{
		if(acfg::exporigin)
		{
			string sXff;
			if(pszOrigin)
			{
				sXff = *pszOrigin;
				sXff += ", ";
			}
			sXff+=m_sClientHost;
			m_pDlClient=new dlcon(false, &sXff);
		}
		else
			m_pDlClient=new dlcon(false);
		
		if(!m_pDlClient)
			return false;
	}
	MYCATCH(bad_alloc&)
	{
		return false;
	}

	if (0==pthread_create(&m_dlerthr, nullptr, _StartDownloader,
			(void *)m_pDlClient))
	{
		return true;
	}
	delete m_pDlClient;
	m_pDlClient=nullptr;
	return false;
}

void con::__tlogstuff::write()
{
	if (sumIn>0)
		aclog::transfer('I', sumIn, client.c_str(), file.c_str());
	
	if(sumOut>0)
		aclog::transfer(bFileIsError ? 'E' : 'O', sumOut, client.c_str(), file.c_str());
}

void con::LogDataCounts(const std::string & sFile, const char *xff, off_t nNewIn,
		off_t nNewOut, bool bFileIsError)
{
	LOGSTART("con::LogDataCounts");
	string sClient;
	if (!acfg::logxff || !xff) // not to be logged or not available
		sClient=m_sClientHost;
	else if (xff)
	{
		sClient=xff;
		trimString(sClient);
		string::size_type pos = sClient.find_last_of(SPACECHARS);
		if (pos!=stmiss)
			sClient.erase(0, pos+1);
	}
	if(sFile != logstuff.file || sClient != logstuff.client)
	{
		logstuff.write();
		logstuff.reset(sFile, sClient, bFileIsError);
	}
	LOG("heh? state now: " << logstuff.sumIn << " " << logstuff.sumOut);
	logstuff.sumIn+=nNewIn;
	logstuff.sumOut+=nNewOut;

}

void con::__tlogstuff::reset(const std::string &pNewFile, const std::string &pNewClient, bool bIsError)
{
	bFileIsError=bIsError;
	file=pNewFile;
	client=pNewClient;
	sumIn=0;
	sumOut=0;
}
