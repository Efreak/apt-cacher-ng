#define LOCAL_DEBUG
#include "debug.h"

#include "config.h"
#include "acfg.h"
#include "dlcon.h"

#include "fileitem.h"
#include "fileio.h"
#include "sockio.h"

using namespace MYSTD;

// evil hack to simulate random disconnects
//#define DISCO_FAILURE

typedef MYSTD::pair<const tHttpUrl*,bool> tHostIsproxy;

static const cmstring sGenericError("567 Unknown download error occured");

dlcon::dlcon(bool bManualExecution, string *xff) :
		m_bStopASAP(false), m_bManualMode(bManualExecution), m_nTempPipelineDisable(0),
		m_bProxyTot(false)
{
	LOGSTART("dlcon::dlcon");
	m_wakepipe[0] = m_wakepipe[1] = -1;
	if (0 == pipe(m_wakepipe))
	{
		set_nb(m_wakepipe[0]);
		set_nb(m_wakepipe[1]);
	}
	if (xff)
		m_sXForwardedFor = *xff;
}

struct tDlJob
{
	tFileItemPtr m_pStorage;
	mstring sErrorMsg;
	dlcon &m_parent;

	inline bool HasBrokenStorage()
	{
		return (!m_pStorage || m_pStorage->GetStatus() > fileitem::FIST_COMPLETE);
	}

#define HINT_MORE 0
#define HINT_DONE 1
#define HINT_DISCON 2
#define EFLAG_JOB_BROKEN 4
#define EFLAG_MIRROR_BROKEN 8
#define EFLAG_STORE_COLLISION 16
#define HINT_SWITCH 32
#define EFLAG_LOST_CON 64
#define HINT_KILL_LAST_FILE 128
#define HINT_TGTCHANGE 256

	inline const tHttpUrl *GetPeerHost()
	{
		return m_pCurBackend ? m_pCurBackend : &m_fileUri;
	}

	inline acfg::tRepoData::IHookHandler * GetConnStateTracker()
	{
		return m_pBEdata ? m_pBEdata->m_pHooks : NULL;
	}

	typedef enum
	{
		STATE_GETHEADER, STATE_REGETHEADER, STATE_PROCESS_DATA,
		STATE_GETCHUNKHEAD, STATE_PROCESS_CHUNKDATA, STATE_GET_CHUNKTRAILER,
		STATE_FINISHJOB
	} tDlState;

	const acfg::tRepoData * m_pBEdata;

	tHttpUrl m_fileUri;
	const tHttpUrl *m_pCurBackend;

	uint_fast8_t m_eReconnectASAP;

	off_t m_nRest;

	tDlState m_DlState;

	int m_nRedirRemaining;

	inline void Init()
	{
		m_nRest = 0;
		m_eReconnectASAP = 0;
		m_DlState = STATE_GETHEADER;
		m_pCurBackend = NULL;
		if (m_pStorage)
			m_pStorage->IncDlRefCount();
	}

	inline tDlJob(dlcon *p, tFileItemPtr pFi, const tHttpUrl& uri, int redirmax) :
			m_pStorage(pFi), m_parent(*p), m_pBEdata(NULL),
			m_fileUri(uri),
			m_nRedirRemaining(redirmax)
	{
		Init();
	}

	inline tDlJob(dlcon *p, tFileItemPtr pFi, const acfg::tRepoData * pBackends,
			const MYSTD::string & sPath, int redirmax) :
			m_pStorage(pFi), m_parent(*p), m_pBEdata(pBackends),
			m_nRedirRemaining(redirmax)
	{
		Init();
		m_fileUri.sPath=sPath;
	}

	~tDlJob()
	{
		if (m_pStorage)
			m_pStorage->DecDlRefCount(sErrorMsg.empty() ? sGenericError : sErrorMsg);
	}

	inline bool RewriteSource(const char *pNewUrl)
	{

		if (--m_nRedirRemaining <= 0)
		{
			sErrorMsg = "500 Bad redirection (loop)";
			return false;
		}

		if (!pNewUrl)
		{
			sErrorMsg = "500 Bad redirection (empty)";
			return false;
		}

		// it's previous server's job to not send any crap. Just make sure that
		// the format is right for further processing here.
		mstring s;
		for (const char *p = pNewUrl; *p; ++p)
		{
			if (isspace((unsigned char) *p) || *p == '%')
			{
				s += "%";
				s += BytesToHexString((uint8_t*) p, 1);
			}
			else
				s += *p;
		}

		m_pBEdata = NULL;
		m_pCurBackend = NULL;
		if (m_fileUri.SetHttpUrl(s))
			m_fileUri.bIsTransferlEncoded=true;
		else
		{
			sErrorMsg = "500 Bad redirection (invalid URL)";
			return false;
		}

		return true;
	}

	inline tHostIsproxy GetConnectHost()
	{
		if (!m_parent.m_bProxyTot)
		{
			// otherwise consider using proxy

			if (m_pBEdata && m_pBEdata->m_pProxy)
			{
				// do what the specific entry says
				if(m_pBEdata->m_pProxy->sHost.empty())
					return make_pair(GetPeerHost(), false);
				return make_pair(m_pBEdata->m_pProxy, true);
			}
			if (!acfg::proxy_info.sHost.empty())
				return make_pair(& acfg::proxy_info, true);
			// ok, no proxy...
		}
		return make_pair(GetPeerHost(), false);
	}

	// needs connectedHost, blacklist, output buffer from the parent, proxy mode?
	inline void AppendRequest(tSS &head, cmstring &xff)
	{
		LOGSTART("tDlJob::AppendRequest");

		head << (m_pStorage->m_bHeadOnly ? "HEAD " : "GET ");

		tHostIsproxy conHostInfo = GetConnectHost();

		if (conHostInfo.second)
		{
			head << RemoteUri(true);
		}
		else // only absolute path without scheme
		{
			if (m_pCurBackend) // base dir from backend definition
				head << UrlEscape(m_pCurBackend->sPath);

			if(m_fileUri.bIsTransferlEncoded)
				head << m_fileUri.sPath;
			else
				head << UrlEscape(m_fileUri.sPath);
		}

		ldbg(RemoteUri(true));

		head << " HTTP/1.1\r\n" << acfg::agentheader << "Host: " << GetPeerHost()->sHost << "\r\n";

		if (conHostInfo.second) // proxy stuff, and add authorization if there is any
		{
			ldbg("using proxy");
			if(!conHostInfo.first->sUserPass.empty())
			{
				head << "Proxy-Authorization: Basic "
						<< EncodeBase64Auth(conHostInfo.first->sUserPass) << "\r\n";
			}
			// Proxy-Connection is a non-sensical copy of Connection but some proxy
			// might listen only to this one so better add it
			head << (acfg::persistoutgoing ? "Proxy-Connection: keep-alive\r\n"
									: "Proxy-Connection: close\r\n");
		}

		const tHttpUrl& pSourceHost(* GetPeerHost());
		if(!pSourceHost.sUserPass.empty())
		{
			head << "Authorization: Basic "
				<< EncodeBase64Auth(pSourceHost.sUserPass) << "\r\n";
		}

		// either by backend or by host in file uri, never both
		ASSERT( (m_pCurBackend && m_fileUri.sHost.empty()) || (!m_pCurBackend && !m_fileUri.sHost.empty()));

		if (m_pStorage->m_nSizeSeen > 0 || m_pStorage->m_nRangeLimit >=0)
		{
			bool bSetRange(false), bSetIfRange(false);

			lockguard g(m_pStorage.get());
			const header &pHead = m_pStorage->GetHeaderUnlocked();

			if (m_pStorage->m_bCheckFreshness)
			{
				if (pHead.h[header::LAST_MODIFIED])
				{
					if (acfg::vrangeops > 0)
					{
						bSetIfRange = true;
						bSetRange = true;
					}
					else if(acfg::vrangeops == 0)
					{
						head << "If-Modified-Since: " << pHead.h[header::LAST_MODIFIED] << "\r\n";
					}
				}
			}
			else
			{
				/////////////// this was protection against broken stuff in the pool ////
				// static file type, date does not matter. check known content length, not risking "range not satisfiable" result
				//
				//off_t nContLen=atol(h.get("Content-Length"));
				//if (nContLen>0 && j->m_pStorage->m_nFileSize < nContLen)
				bSetRange = true;
			}

			/*
			if(m_pStorage->m_nSizeSeen >0 && m_pStorage->m_nRangeLimit>=0)
			{
				bool bSaneRange=m_pStorage->m_nRangeLimit >=m_pStorage->m_nSizeSeen;
				// just to be sure
				ASSERT(bSaneRange);
			}
			if(m_pStorage->m_nRangeLimit < m_pStorage->m_nSizeSeen)
				bSetRange = bSetIfRange = false;
*/

			/* use APT's old trick - set the starting position one byte lower -
			 * this way the server has to send at least one byte if the assumed
			 * position is correct, and we never get a 416 error (one byte
			 * waste is acceptable).
			 * */
			if (bSetRange)
			{
				head << "Range: bytes=" << std::max(off_t(0), m_pStorage->m_nSizeSeen - 1) << "-";
				if(m_pStorage->m_nRangeLimit>=0)
					head << m_pStorage->m_nRangeLimit;
				head << "\r\n";
			}

			if (bSetIfRange)
				head << "If-Range: " << pHead.h[header::LAST_MODIFIED] << "\r\n";
		}

		if (m_pStorage->m_bCheckFreshness)
			head << "Cache-Control: no-store,no-cache,max-age=0\r\n";

		if (acfg::exporigin && !xff.empty())
			head << "X-Forwarded-For: " << xff << "\r\n";

		head << acfg::requestapx << "Accept: */*\r\nConnection: "
				<< (acfg::persistoutgoing ? "keep-alive\r\n\r\n" : "close\r\n\r\n");

#ifdef SPAM
		//head.syswrite(2);
#endif

	}

	inline string RemoteUri(bool bEscaped)
	{
		if(m_pCurBackend)
			return m_pCurBackend->ToURI(bEscaped) +
					( (bEscaped && !m_fileUri.bIsTransferlEncoded)
							?
							UrlEscape(m_fileUri.sPath)
							:
							m_fileUri.sPath);

		return m_fileUri.ToURI(bEscaped);
	}

	uint_fast8_t NewDataHandler(acbuf & inBuf)
	{
		LOGSTART("tDlJob::NewDataHandler");
		while (true)
		{
			off_t nToStore = min((off_t) inBuf.size(), m_nRest);
			ldbg("To store: " <<nToStore);
			if (0 == nToStore)
				break;

			if (!m_pStorage->StoreFileData(inBuf.rptr(), nToStore))
			{
				dbgline;
				sErrorMsg = "502 Could not store data";
				return HINT_DISCON | EFLAG_JOB_BROKEN;
			}

			m_nRest -= nToStore;
			inBuf.drop(nToStore);
		}

		ldbg("Rest: " << m_nRest);

		if (m_nRest == 0)
		{
			m_DlState = (STATE_PROCESS_DATA == m_DlState) ? STATE_FINISHJOB : STATE_GETCHUNKHEAD;
		}
		else
			return HINT_MORE; // will come back

		return HINT_SWITCH;
	}

	/*!
	 *
	 * Process new incoming data and write it down to disk or other receivers.
	 */
	UINT ProcessIncomming(acbuf & inBuf, bool bOnlyRedirectionActivity)
	{
		LOGSTART("tDlJob::ProcessIncomming");
		if (!m_pStorage)
		{
			sErrorMsg = "502 Bad cache descriptor";
			return HINT_DISCON | EFLAG_JOB_BROKEN;
		}

		for (;;) // returned by explicit error (or get-more) return
		{
			ldbg("switch: " << m_DlState);

			if (STATE_GETHEADER == m_DlState ||  STATE_REGETHEADER == m_DlState)
			{
				ldbg("STATE_GETHEADER");
				header h;
				if (inBuf.size() == 0)
					return HINT_MORE;

				bool bHotItem = (m_DlState == STATE_REGETHEADER);
				dbgline;

				int l = h.LoadFromBuf(inBuf.rptr(), inBuf.size());
				if (0 == l)
					return HINT_MORE;
				if (l<0)
				{
					dbgline;
					sErrorMsg = "500 Invalid header";
					// can be followed by any junk... drop that mirror, previous file could also contain bad data
					return EFLAG_MIRROR_BROKEN | HINT_DISCON | HINT_KILL_LAST_FILE;
				}

				ldbg("contents: " << MYSTD::string(inBuf.rptr(), l));
				inBuf.drop(l);
				if (h.type != header::ANSWER)
				{
					dbgline;
					sErrorMsg = "500 Unexpected response type";
					// smells fatal...
					return EFLAG_MIRROR_BROKEN | HINT_DISCON;
				}
				ldbg("GOT, parsed: " << h.frontLine);

				int st = h.getStatus();

				if (acfg::redirmax) // internal redirection might be disabled
				{
					if (st == 301 || st == 302 || st == 307)
					{
						if (RewriteSource(h.h[header::LOCATION]))
						{
							// drop the redirect page contents if possible so the outer loop
							// can scan other headers
							off_t contLen = atoofft(h.h[header::CONTENT_LENGTH], 0);
							if (contLen <= inBuf.size())
								inBuf.drop(contLen);
							return HINT_TGTCHANGE; // no other flags, caller will evaluate the state
						}
						else
							return EFLAG_JOB_BROKEN;
					}

					// for non-redirection responses process as usual

					// unless it's a probe run from the outer loop, in this case we
					// should go no further
					if (bOnlyRedirectionActivity)
						return EFLAG_LOST_CON | HINT_DISCON;
				}

				// explicitly blacklist mirror if key file is missing
				if (st >= 400 && m_pBEdata)
				{
					for (tStrVecIterConst it = m_pBEdata->m_keyfiles.begin();
							it != m_pBEdata->m_keyfiles.end(); ++it)
					{
						if (endsWith(m_fileUri.sPath, *it))
						{
							sErrorMsg = "500 Keyfile missing, mirror blacklisted";
							return HINT_DISCON | EFLAG_MIRROR_BROKEN;
						}
					}
				}

				const char *pCon = h.h[header::CONNECTION];
				if(!pCon)
					pCon = h.h[header::PROXY_CONNECTION];
				if (pCon && 0 == strcasecmp(pCon, "close"))
				{
					ldbg("Peer wants to close connection after request");
					m_eReconnectASAP = HINT_DISCON;
				}

				if (m_pStorage->m_bHeadOnly)
				{
					m_DlState = STATE_FINISHJOB;
				}
				// the only case where we expect a 304
				else if(st == 304 && acfg::vrangeops == 0)
				{
					m_pStorage->SetupComplete();
					m_DlState = STATE_FINISHJOB;
				}
				else if (h.h[header::TRANSFER_ENCODING]
						&& 0 == strcasecmp(h.h[header::TRANSFER_ENCODING], "chunked"))
				{
					m_DlState = STATE_GETCHUNKHEAD;
					h.del(header::TRANSFER_ENCODING); // don't care anymore
				}
				else
				{
					dbgline;
					if (!h.h[header::CONTENT_LENGTH])
					{
						sErrorMsg = "500 Missing Content-Length";
						return HINT_DISCON | EFLAG_JOB_BROKEN;
					}
					// may support such endless stuff in the future but that's too unreliable for now
					m_nRest = atoofft(h.h[header::CONTENT_LENGTH]);
					m_DlState = STATE_PROCESS_DATA;
				}

				// ok, can pass the data to the file handler
				h.set(header::XORIG, RemoteUri(false));
				bool bDoRetry(false);
				if(!m_pStorage->DownloadStartedStoreHeader(h, inBuf.rptr(), bHotItem, bDoRetry))
				{
					if(bDoRetry)
						return EFLAG_LOST_CON | HINT_DISCON; // recoverable

					ldbg("Item dl'ed by others or in error state --> drop it, reconnect");
					m_DlState = STATE_PROCESS_DATA;
					sErrorMsg = "502 Cache descriptor busy";
/*					header xh = m_pStorage->GetHeader();
					if(xh.frontLine.length() > 12)
						sErrorMsg = sErrorMsg + " (" + xh.frontLine.substr(12) + ")";
						*/
					return HINT_DISCON | EFLAG_JOB_BROKEN | EFLAG_STORE_COLLISION;
				}
			}
			else if (m_DlState == STATE_PROCESS_CHUNKDATA || m_DlState ==  STATE_PROCESS_DATA)
			{
				// similar states, just handled differently afterwards
				ldbg("STATE_GETDATA");
				uint_fast8_t res = NewDataHandler(inBuf);
				if (HINT_SWITCH != res)
					return res;
			}
			else if (m_DlState == STATE_FINISHJOB)
			{
				ldbg("STATE_FINISHJOB");
				m_DlState = STATE_GETHEADER;
				m_pStorage->StoreFileData(NULL, 0);
				return HINT_DONE | m_eReconnectASAP;
			}
			else if (m_DlState == STATE_GETCHUNKHEAD)
			{
				ldbg("STATE_GETCHUNKHEAD");
				// came back from reading, drop remaining newlines?
				while (inBuf.size() > 0)
				{
					char c = *(inBuf.rptr());
					if (c != '\r' && c != '\n')
						break;
					inBuf.drop(1);
				}
				const char *crlf(0), *pStart(inBuf.c_str());
				if (!inBuf.size() || NULL == (crlf = strstr(pStart, "\r\n")))
				{
					inBuf.move();
					return HINT_MORE;
				}
				UINT len(0);
				if (1 != sscanf(pStart, "%x", &len))
				{
					sErrorMsg = "500 Invalid data stream";
					return EFLAG_JOB_BROKEN; // hm...?
				}
				inBuf.drop(crlf + 2 - pStart);
				if (len > 0)
				{
					m_nRest = len;
					m_DlState = STATE_PROCESS_CHUNKDATA;
				}
				else
					m_DlState = STATE_GET_CHUNKTRAILER;
			}
			else if (m_DlState == STATE_GET_CHUNKTRAILER)
			{
				if (inBuf.size() < 2)
					return HINT_MORE;
				const char *pStart(inBuf.c_str());
				const char *crlf(strstr(pStart, "\r\n"));
				if (!crlf)
					return HINT_MORE;

				if (crlf == pStart) // valid empty line -> done here
				{
					inBuf.drop(2);
					m_DlState = STATE_FINISHJOB;
				}
				else
					inBuf.drop(crlf + 2 - pStart); // drop line and watch for others
			}
		}
		ASSERT(!"unreachable");
		sErrorMsg = "502 Invalid state";
		return EFLAG_JOB_BROKEN;
	}

	inline bool IsRecoverableState()
	{
		return (m_DlState == STATE_GETHEADER || m_DlState == STATE_REGETHEADER);
		// XXX: In theory, could also easily recover from STATE_FINISH but that's
		// unlikely to happen

	}

private:
	// not to be copied ever
	tDlJob(const tDlJob&);
	tDlJob & operator=(const tDlJob&);
};


inline void dlcon::BlacklistMirror(tDlJobPtr & job, cmstring &msg)
{
	LOGSTART2("dlcon::BlacklistMirror", "blacklisting " <<
			job->GetPeerHost()->ToURI(false));
	m_blacklist[make_pair(job->GetPeerHost()->sHost, job->GetPeerHost()->GetPort())] = msg;
}


inline bool dlcon::SetupJobConfig(tDlJobPtr &job, mstring *pReasonMsg)
{
	LOGSTART("dlcon::SetupJobConfig");

	// using backends? Find one which is not blacklisted

	MYMAP<MYSTD::pair<cmstring,cmstring>, mstring>::const_iterator bliter;

	if (job->m_pBEdata)
	{
		// keep the existing one if possible
		if (job->m_pCurBackend)
		{
			LOG("Checking [" << job->m_pCurBackend->sHost << "]:" << job->m_pCurBackend->GetPort());
			bliter = m_blacklist.find(make_pair(job->m_pCurBackend->sHost, job->m_pCurBackend->GetPort()));
			if(bliter == m_blacklist.end())
				return true;
		}

		for (vector<tHttpUrl>::const_iterator it=job->m_pBEdata->m_backends.begin();
				it!=job->m_pBEdata->m_backends.end(); ++it)
		{
			bliter = m_blacklist.find(make_pair(it->sHost, it->GetPort()));
			if(bliter == m_blacklist.end())
			{
				job->m_pCurBackend = &(*it);
				return true;
			}

			if(pReasonMsg)
				*pReasonMsg = bliter->second;
		}
		return false;
	}

	// ok, look for the mirror data itself
	bliter = m_blacklist.find(make_pair(job->GetPeerHost()->sHost, job->GetPeerHost()->GetPort()));
	if(bliter == m_blacklist.end())
		return true;
	else
	{
		if(pReasonMsg)
			*pReasonMsg = bliter->second;
		return false;
	}
}

inline void dlcon::EnqJob(tDlJob *todo)
{
	setLockGuard;

	ASSERT(todo);
	ASSERT(todo->m_pStorage->m_nRangeLimit <0
			|| todo->m_pStorage->m_nRangeLimit >= todo->m_pStorage->m_nSizeSeen);

	LOGSTART2("dlcon::EnqJob", todo->m_fileUri.ToURI(false));

	m_qNewjobs.push_back(tDlJobPtr(todo));
	if (m_wakepipe[1]>=0)
		POKE(m_wakepipe[1]);
}

void dlcon::AddJob(tFileItemPtr m_pItem, 
		const acfg::tRepoData *pBackends, const MYSTD::string & sPatSuffix)
{
	EnqJob(new tDlJob(this, m_pItem, pBackends, sPatSuffix,
			m_bManualMode ? ACFG_REDIRMAX_DEFAULT : acfg::redirmax));
}

void dlcon::AddJob(tFileItemPtr m_pItem, tHttpUrl hi)
{
	EnqJob(new tDlJob(this, m_pItem, hi,
			m_bManualMode ? ACFG_REDIRMAX_DEFAULT : acfg::redirmax));
}

void dlcon::SignalStop()
{
	LOGSTART("dlcon::SignalStop");
	setLockGuard;

	// stop all activity as soon as possible
	m_bStopASAP=true;
	m_qNewjobs.clear();

	POKE(m_wakepipe[1]);
}

dlcon::~dlcon()
{
	LOGSTART("dlcon::~dlcon, Destroying dlcon");
	checkforceclose(m_wakepipe[0]);
	checkforceclose(m_wakepipe[1]);
}

inline UINT dlcon::ExchangeData(mstring &sErrorMsg, tTcpHandlePtr &con, tDljQueue &inpipe)
{
	LOGSTART2("dlcon::ExchangeData",
			"qsize: " << inpipe.size() << ", sendbuf size: "
			<< m_sendBuf.size() << ", inbuf size: " << m_inBuf.size());

	fd_set rfds, wfds;
	struct timeval tv;
	int r = 0;
	int fd = con ? con->GetFD() : -1;
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);

	if(inpipe.empty())
		m_inBuf.clear(); // better be sure about dirty buffer from previous connection

	// no socket operation needed in this case but just process old buffer contents
	bool bReEntered=!m_inBuf.empty();

	loop_again:

	for (;;)
	{
		FD_SET(m_wakepipe[0], &rfds);
		int nMaxFd = m_wakepipe[0];

		if (fd>=0)
		{
			FD_SET(fd, &rfds);
			nMaxFd = MYSTD::max(fd, nMaxFd);

			if (!m_sendBuf.empty())
			{
				ldbg("Needs to send " << m_sendBuf.size() << " bytes");
				FD_SET(fd, &wfds);
			}
#ifdef HAVE_SSL
			else if(con->GetBIO() && BIO_should_write(con->GetBIO()))
			{
				FD_SET(fd, &wfds);
			}
#endif
		}

		ldbg("select dlcon");
		tv.tv_sec = acfg::nettimeout;
		tv.tv_usec = 0;

		// jump right into data processing but only once
		if(bReEntered)
		{
			bReEntered=false;
			goto proc_data;
		}

		r=select(nMaxFd + 1, &rfds, &wfds, NULL, &tv);

		if (r < 0)
		{
			dbgline;
			if (EINTR == errno)
				continue;
#ifdef MINIBUILD
			string fer("select failed");
#else
			errnoFmter fer("FAILURE: select, ");
			LOG(fer);
#endif
			sErrorMsg = string("500 Internal malfunction, ") + fer;
			return HINT_DISCON|EFLAG_JOB_BROKEN|EFLAG_MIRROR_BROKEN;
		}
		else if (r == 0) // looks like a timeout
		{
			dbgline;
			sErrorMsg = "500 Connection timeout";
			// was there anything to do at all?
			if(inpipe.empty())
				return HINT_SWITCH;

			if(inpipe.front()->IsRecoverableState())
				return EFLAG_LOST_CON;
			else
				return (HINT_DISCON|EFLAG_JOB_BROKEN);
		}

		if (FD_ISSET(m_wakepipe[0], &rfds))
		{
			dbgline;
			for (int tmp; read(m_wakepipe[0], &tmp, 1) > 0;)
				;
			return HINT_SWITCH;
		}

		if (fd>=0)
		{
#ifdef HAVE_SSL
			if (con->GetBIO())
			{
				int s=BIO_write(con->GetBIO(), m_sendBuf.rptr(), m_sendBuf.size());
				if(s>0)
					m_sendBuf.drop(s);
			}
			else
#endif
			if (FD_ISSET(fd, &wfds))
			{
				FD_CLR(fd, &wfds);

				ldbg("Sending data...\n" << m_sendBuf);
				int s = ::send(fd, m_sendBuf.data(), m_sendBuf.length(), MSG_NOSIGNAL);
				ldbg("Sent " << s << " bytes from " << m_sendBuf.length() << " to " << con.get());
				if (s < 0)
				{
					// EAGAIN is weird but let's retry later, otherwise reconnect
					if (errno != EAGAIN && errno != EINTR)
					{
						sErrorMsg = "502 Send failed";
						return EFLAG_LOST_CON;
					}
				}
				else
					m_sendBuf.drop(s);
			}
		}

		if (fd >=0 && (FD_ISSET(fd, &rfds)
#ifdef HAVE_SSL
				|| (con->GetBIO() && BIO_should_read(con->GetBIO()))
#endif
			))
		{
#ifdef HAVE_SSL
			if(con->GetBIO())
			{
				r=BIO_read(con->GetBIO(), m_inBuf.wptr(), m_inBuf.freecapa());
				if(r>0)
					m_inBuf.got(r);
				else // <=0 doesn't mean an error, only a double check can tell
					r=BIO_should_read(con->GetBIO()) ? 1 : -errno;
			}
			else
#endif
			{
				r = m_inBuf.sysread(fd);
			}

#ifdef DISCO_FAILURE
#warning hej
			static int fakeFail=3;
			if( fakeFail-- < 0)
			{
//				LOGLVL(LOG_DEBUG, "\n#################\nFAKING A FAILURE\n###########\n");
				r=0;
				fakeFail=10;
				errno = EROFS;
				r = -errno;
			}
#endif

			if(r == -EAGAIN || r == -EWOULDBLOCK)
			{
				ldbg("why EAGAIN/EINTR after getting it from select?");
//				timespec sleeptime = { 0, 432000000 };
//				nanosleep(&sleeptime, NULL);
				goto loop_again;
			}
			else if (r == 0)
			{
				dbgline;
				sErrorMsg = "502 Connection closed";
				return EFLAG_LOST_CON;
			}
			else if (r < 0) // other error, might reconnect
			{
				dbgline;
#ifdef MINIBUILD
				sErrorMsg = "502 EPIC FAIL";
#else
				// pickup the error code for later and kill current connection ASAP
				sErrorMsg = errnoFmter("502 ");
#endif
				return EFLAG_LOST_CON;
			}

			proc_data:

			if(inpipe.empty())
			{
				ldbg("FIXME: unexpected data returned?");
				sErrorMsg = "500 Unexpected data";
				return EFLAG_LOST_CON;
			}

			while(!m_inBuf.empty())
			{

				ldbg("Processing job for " << inpipe.front()->RemoteUri(false));
				UINT res = inpipe.front()->ProcessIncomming(m_inBuf, false);
				ldbg(
						"... incoming data processing result: " << res
						<< ", emsg: " << inpipe.front()->sErrorMsg);

				if(res&EFLAG_MIRROR_BROKEN)
				{
					ldbg("###### BROKEN MIRROR ####### on " << con.get());
				}

				if (HINT_MORE == res)
					goto loop_again;

				if (HINT_DONE & res)
				{

					// just in case that server damaged the last response body
					con->KnowLastFile(WEAK_PTR<fileitem>(inpipe.front()->m_pStorage));

					inpipe.pop_front();
					if (HINT_DISCON & res)
						return HINT_DISCON; // with cleaned flags

					LOG(
							"job finished. Has more? " << inpipe.size()
							<< ", remaining data? " << m_inBuf.size());

					if (inpipe.empty())
					{
						LOG("Need more work");
						return HINT_SWITCH;
					}

					LOG("Extract more responses");
					continue;
				}

				if (HINT_TGTCHANGE & res)
				{
					/* If the target was modified for internal redirection then there might be
					 * more responses of that kind in the queue. Apply the redirection handling
					 * to the rest as well if possible without having side effects.
					 */
					tDljQueue::iterator it = inpipe.begin();
					for(++it; it != inpipe.end(); ++it)
					{
						UINT rr = (**it).ProcessIncomming(m_inBuf, true);
						// just the internal rewriting applied and nothing else?
						if( HINT_TGTCHANGE != rr )
						{
							// not internal redirection or some failure doing it
							m_nTempPipelineDisable=30;
							return (HINT_TGTCHANGE|HINT_DISCON);
						}
					}
					// processed all inpipe stuff but if the buffer is still not empty then better disconnect
					return HINT_TGTCHANGE | (m_inBuf.empty() ? 0 : HINT_DISCON);
				}

				// else case: error handling, pass to main loop
				if(HINT_KILL_LAST_FILE & res)
					con->KillLastFile();
				setIfNotEmpty(sErrorMsg, inpipe.front()->sErrorMsg);
				return res;
			}
			return HINT_DONE; // input buffer consumed
		}
	}

	ASSERT(!"Unreachable");
	sErrorMsg = "500 Internal failure";
	return EFLAG_JOB_BROKEN|HINT_DISCON;
}

inline void CleanRunning(tDljQueue &inpipe)
{
	for(tDljQueue::iterator it = inpipe.begin(); it!= inpipe.end();)
	{
		if(*it && (**it).m_pStorage
				&& (**it).m_pStorage->GetStatus() >= fileitem::FIST_DLRECEIVING)
		{
			// someone else is doing it -> drop
			inpipe.erase(it++);
			continue;
		}
		else
			++it;
	}
}

void dlcon::WorkLoop()
{
	LOGSTART("dlcon::WorkLoop");
    string sErrorMsg;
    m_inBuf.clear();
    
	if (!m_inBuf.setsize(acfg::dlbufsize))
	{
		aclog::err("500 Out of memory");
		return;
	}

	if(m_wakepipe[0]<0 || m_wakepipe[1]<0)
	{
		aclog::err("Error creating pipe file descriptors");
		return;
	}

	tDljQueue inpipe;
	tTcpHandlePtr con;
	UINT loopRes=0;

	bool bStopRequesting=false; // hint to stop adding request headers until the connection is restarted

	int nLostConTolerance=0;
#define MAX_RETRY 5

	while(true) // outer loop: jobs, connection handling
	{
        // init state or transfer loop jumped out, what are the needed actions?
        {
        	setLockGuard;
        	LOG("New jobs: " << m_qNewjobs.size());

        	if(m_bStopASAP)
        	{
        		/* The no-more-users checking logic will purge orphaned items from the inpipe
        		 * queue. When the connection is dirty after that, it will be closed in the
        		 * ExchangeData() but if not then it can be assumed to be clean and reusable.
        		 */
        		if(inpipe.empty())
        		{
        			if(con)
        				tcpconnect::RecycleIdleConnection(con);
        			return;
        		}
        	}


        	if(m_qNewjobs.empty())
        		goto go_select; // parent will notify RSN

        	if(!con)
        	{
        		// cleanup after the last connection - send buffer, broken jobs, ...
        		m_sendBuf.clear();
        		m_inBuf.clear();
        		inpipe.clear();

        		bStopRequesting=false;

        		for(tDljQueue::iterator it=m_qNewjobs.begin(); it!=m_qNewjobs.end();)
        		{
        			if(SetupJobConfig(*it, &sErrorMsg))
        				++it;
        			else
        			{
        				setIfNotEmpty2( (**it).sErrorMsg, sErrorMsg,
        						"500 Broken mirror or incorrect configuration");
        				m_qNewjobs.erase(it++);
        			}
        		}
        		if(m_qNewjobs.empty())
        		{
        			LOG("no jobs left, start waiting")
        			goto go_select; // nothing left, might receive new jobs soon
        		}

        		bool bUsed=false;
#ifdef HAVE_SSL
#define doconnect(x, t) tcpconnect::CreateConnected(x->sHost, x->GetPort(), \
				sErrorMsg, &bUsed, m_qNewjobs.front()->GetConnStateTracker(), x->bSSL, t)
#else
#define doconnect(x, t) tcpconnect::CreateConnected(x->sHost, x->GetPort(), \
				sErrorMsg, &bUsed, m_qNewjobs.front()->GetConnStateTracker(), false, t)

#endif
        		tHostIsproxy conHost = m_qNewjobs.front()->GetConnectHost();
        		con = doconnect( conHost.first, (conHost.second && acfg::optproxytimeout>0)
        				? acfg::optproxytimeout : acfg::nettimeout);

        		if(!con && acfg::optproxytimeout>0)
        		{
        			ldbg("optional proxy broken, disable");
        			m_bProxyTot=true;
        			con = doconnect(m_qNewjobs.front()->GetPeerHost(), acfg::nettimeout);
        		}

        		nLostConTolerance = MAX_RETRY + bUsed;

        		ldbg("connection valid? " << bool(con) << " was fresh? " << !bUsed);

        		if(con)
        		{
        			ldbg("target? [" << con->GetHostname() << "]:" << con->GetPort());

        			// must test this connection, just be sure no crap is in the pipe
        			if (bUsed && check_read_state(con->GetFD()))
        			{
        				ldbg("code: MoonWalker");
        				con.reset();
        				continue;
        			}
        		}
        		else
        		{
        			BlacklistMirror(m_qNewjobs.front(), sErrorMsg);
        			continue; // try the next backend
        		}
        	}

        	// connection should be stable now, prepare all jobs and/or move to pipeline
        	while(!bStopRequesting
        			&& !m_qNewjobs.empty()
        			&& int(inpipe.size()) <= acfg::pipelinelen)
        	{
   				tDlJobPtr &cjob = m_qNewjobs.front();

        		bool bGoodConfig = SetupJobConfig(cjob, NULL);

        		/*
        		ldbg("target: " << cjob->GetPeerName() << " vs " << con->GetHostname()
        				<< ", ports: " << cjob->GetPeerPort() << " vs " << con->GetPort()
        				<< ", good config: " << bGoodConfig);
*/

        		if(!bGoodConfig)
        		{
        			// something weird happened to it, drop it and let the client care
        			m_qNewjobs.pop_front();
        			continue;
        		}

				// needs to send them for the connected target host
        		tHostIsproxy hostNew=cjob->GetConnectHost();
        		if(hostNew.first->sHost != con->GetHostname() ||
        				hostNew.first->GetPort() != con->GetPort())
        		{
        			LOG("host mismatch," << hostNew.first->sHost << ":" <<
        					hostNew.first->GetPort() <<
        					" vs. " << con->GetHostname() << ":"<<con->GetPort() <<
        					" -- stop sending requesting for now");
        			bStopRequesting=true;
        			break;
        		}

				cjob->AppendRequest(m_sendBuf, m_sXForwardedFor);
				LOG("request added to buffer");
				inpipe.push_back(cjob);
				m_qNewjobs.pop_front();

				if (m_nTempPipelineDisable > 0)
				{
					bStopRequesting = true;
					--m_nTempPipelineDisable;
					break;
				}

        	}
        }

		ldbg("Request(s) cooked, buffer contents: " << m_sendBuf);

        go_select:

        if(inpipe.empty() && m_bManualMode)
        {
        	return;
        }

        // inner loop: plain communication until something happens. Maybe should use epoll here?
        loopRes=ExchangeData(sErrorMsg, con, inpipe);
        ldbg("loopRes: "<< loopRes);

        /* check whether we have a pipeline stall. This may happen because a) we are done or
         * b) because of the remote hostname change or c) the client stopped sending tasks.
         * Anyhow, that's a reason to put the connection back into the shared pool so either we
         * get it back from the pool in the next workloop cycle or someone else gets it and we
         * get a new connection for the new host later.
         * */
        if (inpipe.empty())
		{
        	// all requests have been processed (client done, or pipeline stall, who cares)
			dbgline;

			// no matter what happened, that stop flag is now irrelevant
			bStopRequesting = false;

			// no error bits set, not busy -> this connection is still good, recycle properly
			UINT all_err = HINT_DISCON | EFLAG_JOB_BROKEN | EFLAG_LOST_CON | EFLAG_MIRROR_BROKEN;
			if (con && !(loopRes & all_err))
			{
				dbgline;
				tcpconnect::RecycleIdleConnection(con);
				continue;
			}
		}


        /*
         * Here we go if the inpipe is still not processed or there have been errors
         * needing special handling.
         */

        if( (HINT_DISCON|EFLAG_LOST_CON) & loopRes)
        {
        	dbgline;
        	con.reset();
        	m_inBuf.clear();
        	m_sendBuf.clear();
        }

        if ( loopRes & HINT_TGTCHANGE )
        {
        	// short queue continues jobs with rewritten targets, so
        	// reinsert them into the new task list and continue

        	// if conn was not reset above then it should be in good shape
        	tcpconnect::RecycleIdleConnection(con);
        	goto move_jobs_back_to_q;
        }

        if ((EFLAG_LOST_CON & loopRes) && !inpipe.empty())
		{
			// disconnected by OS... give it a chance, or maybe not...
			if (--nLostConTolerance <= 0)
				BlacklistMirror(inpipe.front(), sErrorMsg);

			timespec sleeptime = { 0, 325000000 };
			nanosleep(&sleeptime, NULL);

			// trying to resume that job secretly, unless user disabled the use of range (we
			// cannot resync the sending position ATM, throwing errors to user for now)
			if (acfg::vrangeops <= 0 && inpipe.front()->m_pStorage->m_bCheckFreshness)
				loopRes |= EFLAG_JOB_BROKEN;
			else
				inpipe.front()->m_DlState = tDlJob::STATE_REGETHEADER;
		}

        if(loopRes & (HINT_DONE|HINT_MORE))
        {
        	nLostConTolerance=MAX_RETRY;
        	sErrorMsg.clear();
        	continue;
        }

        //
        // regular required post-processing done here, now handle special conditions
        //


        if(HINT_SWITCH == loopRes)
        	continue;

        // resolving the "fatal error" situation, push the pipelined job back to new, etc.

        if( (EFLAG_MIRROR_BROKEN & loopRes) && !inpipe.empty())
        	BlacklistMirror(inpipe.front(), sErrorMsg);

        if( (EFLAG_JOB_BROKEN & loopRes) && !inpipe.empty())
        {
        	setIfNotEmpty(inpipe.front()->sErrorMsg, sErrorMsg);

        	inpipe.pop_front();

        	if(EFLAG_STORE_COLLISION & loopRes)
        	{
				// stupid situation, both users downloading the same stuff - and most likely in the same order
				// if one downloader runs a step ahead (or usually many steps), drop all items
				// already processed by it and try to continue somewhere else.
				// This way, the overall number of collisions and reconnects is minimized

        		CleanRunning(inpipe); // seriously, I want lambdas
        		setLockGuard;
        		CleanRunning(m_qNewjobs);
        	}
        }

        move_jobs_back_to_q:
        // for the jobs that were not finished and/or dropped, move them back into the task queue
        {
        	setLockGuard;
        	m_qNewjobs.insert(m_qNewjobs.begin(), inpipe.begin(), inpipe.end());
        	inpipe.clear();
        }

	}
}
