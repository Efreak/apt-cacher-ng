
//#include <netinet/in.h>
//#include <netdb.h>

#include "tcpconnect.h"
#include "lockable.h"
#include "fileitem.h"
#include "acfg.h"
#include "acbuf.h"

#include <unistd.h>
#include <sys/time.h>
#include <atomic>
#include <algorithm>
#include <list>

#define LOCAL_DEBUG
#include "debug.h"

#include "config.h"
#include "acfg.h"
#include "dlcon.h"

#include "fileitem.h"
#include "fileio.h"
#include "sockio.h"

#ifdef HAVE_LINUX_EVENTFD
#include <sys/eventfd.h>
#endif

using namespace std;

// evil hack to simulate random disconnects
//#define DISCO_FAILURE

#define MAX_RETRY cfg::dlretriesmax

namespace acng
{

static cmstring sGenericError("567 Unknown download error occured");

// those are not allowed to be forwarded
static const auto taboo =
{
	string("Host"), string("Cache-Control"),
	string("Proxy-Authorization"), string("Accept"),
	string("User-Agent")
};

std::atomic_uint g_nDlCons(0);


class dlcon::Impl
{

	struct tDlJob;
	typedef std::list<tDlJob> tDljQueue;
	friend struct tDlJob;
	friend class dlcon;

	tDljQueue m_new_jobs;
	const IDlConFactory &m_conFactory;
	std::string m_ownersHostname;

#ifdef HAVE_LINUX_EVENTFD
	int m_wakeventfd = -1;
#define fdWakeRead m_wakeventfd
#define fdWakeWrite m_wakeventfd
#else
	int m_wakepipe[2] =
	{ -1, -1 };
#define fdWakeRead m_wakepipe[0]
#define fdWakeWrite m_wakepipe[1]
#endif
	atomic_int m_ctrl_hint = ATOMIC_VAR_INIT(0);
	mutex m_handover_mutex;

	/// blacklist for permanently failing hosts, with error message
	std::map<std::pair<cmstring, cmstring>, mstring> m_blacklist;
	tSS m_sendBuf, m_inBuf;

	unsigned ExchangeData(mstring &sErrorMsg, tDlStreamHandle &con,
			tDljQueue &qActive);

	// Disable pipelining for the next # requests. Actually used as crude workaround for the
	// concept limitation (because of automata over a couple of function) and its
	// impact on download performance.
	// The use case: stupid web servers that redirect all requests do that step-by-step, i.e.
	// they get a bunch of requests but return only the first response and then flush the buffer
	// so we process this response and wish to switch to the new target location (dropping
	// the current connection because we don't keep it somehow to background, this is the only
	// download agent we have). This manner perverts the whole principle and causes permanent
	// disconnects/reconnects. In this case, it's beneficial to disable pipelining and send
	// our requests one-by-one. This is done for a while (i.e. the valueof(m_nDisablePling)/2 )
	// times before the operation mode returns to normal.
	int m_nTempPipelineDisable;

	// the default behavior or using or not using the proxy. Will be set
	// if access proxies shall no longer be used.
	bool m_bProxyTot;

	// this is a binary factor, meaning how many reads from buffer are OK when
	// speed limiting is enabled
	unsigned m_nSpeedLimiterRoundUp = (unsigned(1) << 16) - 1;
	unsigned m_nSpeedLimitMaxPerTake = MAX_VAL(unsigned);
	unsigned m_nLastDlCount = 0;

	void wake();
	void awaken_check();

    void WorkLoop();
    bool AddJob(tFileItemPtr m_pItem, const tHttpUrl *pForcedUrl,
    		const cfg::tRepoData *pRepoDesc,
    		cmstring *sPatSuffix, LPCSTR reqHead,
			int nMaxRedirection, const char* szHeaderXff);

struct tDlJob
{
	tFileItemPtr m_pStorage;
	mstring sErrorMsg;
	dlcon::Impl &m_parent;

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

	const cfg::tRepoData * m_pRepoDesc=nullptr;

	/*!
	 * Returns a reference to http url where host and port and protocol match the current host
	 * Other fields in that member have undefined contents. ;-)
	 */
	inline const tHttpUrl& GetPeerHost()
	{
		return m_pCurBackend ? *m_pCurBackend : m_remoteUri;
	}

	inline cfg::tRepoData::IHookHandler * GetConnStateTracker()
	{
		return m_pRepoDesc ? m_pRepoDesc->m_pHooks : nullptr;
	}

	typedef enum : char
	{
		STATE_GETHEADER, STATE_REGETHEADER, STATE_PROCESS_DATA,
		STATE_GETCHUNKHEAD, STATE_PROCESS_CHUNKDATA, STATE_GET_CHUNKTRAILER,
		STATE_FINISHJOB
	} tDlState;

	string m_extraHeaders, m_xff;

	tHttpUrl m_remoteUri;
	const tHttpUrl *m_pCurBackend=nullptr;

	uint_fast8_t m_eReconnectASAP =0;
	bool m_bBackendMode=false;

	off_t m_nRest =0;

	tDlState m_DlState = STATE_GETHEADER;

	int m_nRedirRemaining;

	inline tDlJob(dlcon::Impl *p, tFileItemPtr pFi,
			const tHttpUrl *pUri,
			const cfg::tRepoData * pRepoData,
			const std::string *psPath,
			int redirmax) :
			m_pStorage(pFi),
			m_parent(*p),
			m_pRepoDesc(pRepoData),
			m_nRedirRemaining(redirmax)
	{
		LOGSTART("tDlJob::tDlJob");
		ldbg("uri: " << (pUri ? pUri->ToURI(false) :  sEmptyString )
				<< ", " << "restpath: " << (psPath?*psPath:sEmptyString)
		<< "repo: " << uintptr_t(pRepoData)
		);
		if (m_pStorage)
			m_pStorage->IncDlRefCount();
		if(pUri)
			m_remoteUri=*pUri;
		else
		{
			m_remoteUri.sPath=*psPath;
			m_bBackendMode=true;
		}
	}
	// Default move ctor is ok despite of pointers, we only need it in the beginning, list-splice operations should not move the object around
	tDlJob(tDlJob && other) = default;

	~tDlJob()
	{
		LOGSTART("tDlJob::~tDlJob");
		if (m_pStorage)
			m_pStorage->DecDlRefCount(sErrorMsg.empty() ? sGenericError : sErrorMsg);
	}

	inline void ExtractCustomHeaders(LPCSTR reqHead)
	{
		if(!reqHead)
			return;
		header h;
		bool forbidden=false;
		h.Load(reqHead, (unsigned) std::numeric_limits<int>::max(),
				[this, &forbidden](cmstring& key, cmstring& rest)
				{
			// heh, continuation of ignored stuff or without start?
			if(key.empty() && (m_extraHeaders.empty() || forbidden))
				return;
			forbidden = taboo.end() != std::find_if(taboo.begin(), taboo.end(),
					[&key](cmstring &x){return scaseequals(x,key);});
			if(!forbidden)
				m_extraHeaders += key + rest;
				}
		);
	}

	inline string RemoteUri(bool bUrlEncoded)
	{
		if(m_pCurBackend)
			return m_pCurBackend->ToURI(bUrlEncoded) +
					( bUrlEncoded ? UrlEscape(m_remoteUri.sPath)
							: m_remoteUri.sPath);

		return m_remoteUri.ToURI(bUrlEncoded);
	}

	inline bool RewriteSource(const char *pNewUrl)
	{
		LOGSTART("tDlJob::RewriteSource");
		if (--m_nRedirRemaining <= 0)
		{
			sErrorMsg = "500 Bad redirection (loop)";
			return false;
		}

		if (!pNewUrl || !*pNewUrl)
		{
			sErrorMsg = "500 Bad redirection (empty)";
			return false;
		}

		// start modifying the target URL, point of no return
		m_pCurBackend = nullptr;
		bool bWasBeMode = m_bBackendMode;
		m_bBackendMode = false;
		sErrorMsg = "500 Bad redirection (path)";

		auto sLocationDecoded = UrlUnescape(pNewUrl);

		tHttpUrl newUri;
		if (newUri.SetHttpUrl(sLocationDecoded, false))
		{
			dbgline;
			m_remoteUri = newUri;
			return true;
		}
		// ok, some protocol-relative crap? let it parse the hostname but keep the protocol
		if (startsWithSz(sLocationDecoded, "//"))
		{
			stripPrefixChars(sLocationDecoded, "/");
			return m_remoteUri.SetHttpUrl(
					m_remoteUri.GetProtoPrefix() + sLocationDecoded);
		}

		// recreate the full URI descriptor matching the last download
		if(bWasBeMode)
		{
			if(!m_pCurBackend)
				return false;
			auto sPathBackup=m_remoteUri.sPath;
			m_remoteUri=*m_pCurBackend;
			m_remoteUri.sPath+=sPathBackup;
		}

		if (startsWithSz(sLocationDecoded, "/"))
		{
			m_remoteUri.sPath = sLocationDecoded;
			return true;
		}
		// ok, must be relative
		m_remoteUri.sPath+=(sPathSepUnix+sLocationDecoded);
		return true;
	}

	bool SetupJobConfig(mstring& sReasonMsg, decltype(dlcon::Impl::m_blacklist) &blacklist)
	{
		LOGSTART("dlcon::Impl::SetupJobConfig");

		// using backends? Find one which is not blacklisted
		if (m_bBackendMode)
		{
			// keep the existing one if possible
			if (m_pCurBackend)
			{
				LOG(
						"Checking [" << m_pCurBackend->sHost << "]:" << m_pCurBackend->GetPort());
				const auto bliter = blacklist.find(
						make_pair(m_pCurBackend->sHost, m_pCurBackend->GetPort()));
				if (bliter == blacklist.end())
					return true;
			}

			// look in the constant list, either it's usable or it was blacklisted before
			for (const auto& bend : m_pRepoDesc->m_backends)
			{
				const auto bliter = blacklist.find(make_pair(bend.sHost, bend.GetPort()));
				if (bliter == blacklist.end())
				{
					m_pCurBackend = &bend;
					return true;
				}

				// uh, blacklisted, remember the last reason
				sReasonMsg = bliter->second;
			}
			return false;
		}

		// ok, not backend mode. Check the mirror data (vs. blacklist)
		auto bliter = blacklist.find(
				make_pair(GetPeerHost().sHost, GetPeerHost().GetPort()));
		if (bliter == blacklist.end())
			return true;

		sReasonMsg = bliter->second;
		return false;
	}

	// needs connectedHost, blacklist, output buffer from the parent, proxy mode?
	inline void AppendRequest(tSS &head, const tHttpUrl *proxy)
	{
		LOGSTART("tDlJob::AppendRequest");

		head << (m_pStorage->m_bHeadOnly ? "HEAD " : "GET ");

		if (proxy)
			head << RemoteUri(true);
		else // only absolute path without scheme
		{
			if (m_pCurBackend) // base dir from backend definition
				head << UrlEscape(m_pCurBackend->sPath);

			head << UrlEscape(m_remoteUri.sPath);
		}

		ldbg(RemoteUri(true));

		head << " HTTP/1.1\r\n" << cfg::agentheader << "Host: " << GetPeerHost().sHost << "\r\n";

		if (proxy) // proxy stuff, and add authorization if there is any
		{
			ldbg("using proxy");
			if(!proxy->sUserPass.empty())
			{
				head << "Proxy-Authorization: Basic "
						<< EncodeBase64Auth(proxy->sUserPass) << "\r\n";
			}
			// Proxy-Connection is a non-sensical copy of Connection but some proxy
			// might listen only to this one so better add it
			head << (cfg::persistoutgoing ? "Proxy-Connection: keep-alive\r\n"
									: "Proxy-Connection: close\r\n");
		}

		const auto& pSourceHost = GetPeerHost();
		if(!pSourceHost.sUserPass.empty())
		{
			head << "Authorization: Basic "
				<< EncodeBase64Auth(pSourceHost.sUserPass) << "\r\n";
		}

		// either by backend or by host in file uri, never both
		//XXX: still needed? Checked while inserting already.
		// ASSERT( (m_pCurBackend && m_fileUri.sHost.empty()) || (!m_pCurBackend && !m_fileUri.sHost.empty()));

		if (m_pStorage->m_nSizeSeen > 0 || m_pStorage->m_nRangeLimit >=0)
		{
			bool bSetRange(false), bSetIfRange(false);

			lockguard g(m_pStorage.get());
			const header &pHead = m_pStorage->GetHeaderUnlocked();

			if (m_pStorage->m_bCheckFreshness)
			{
				if (pHead.h[header::LAST_MODIFIED])
				{
					if (cfg::vrangeops > 0)
					{
						bSetIfRange = true;
						bSetRange = true;
					}
					else if(cfg::vrangeops == 0)
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

		if (cfg::exporigin && !m_xff.empty())
			head << "X-Forwarded-For: " << m_xff << "\r\n";

		head << cfg::requestapx
				<< m_extraHeaders
				<< "Accept: application/octet-stream\r\n"
				"Accept-Encoding: identity\r\n"
				"Connection: "
				<< (cfg::persistoutgoing ? "keep-alive\r\n\r\n" : "close\r\n\r\n");

#ifdef SPAM
		//head.syswrite(2);
#endif

	}

	inline uint_fast8_t NewDataHandler(acbuf & inBuf)
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

		if (m_nRest != 0)
			return HINT_MORE; // will come back

		m_DlState = (STATE_PROCESS_DATA == m_DlState) ? STATE_FINISHJOB : STATE_GETCHUNKHEAD;
		return HINT_SWITCH;
	}

	/*!
	 *
	 * Process new incoming data and write it down to disk or other receivers.
	 */
	unsigned ProcessIncomming(acbuf & inBuf, bool bOnlyRedirectionActivity)
	{
		LOGSTART("tDlJob::ProcessIncomming");
		if (!m_pStorage)
		{
			sErrorMsg = "502 Bad cache descriptor";
			return HINT_DISCON | EFLAG_JOB_BROKEN;
		}

		for (;;) // returned by explicit error (or get-more) return
		{
			ldbg("switch: " << (int)m_DlState);

			if (STATE_GETHEADER == m_DlState ||  STATE_REGETHEADER == m_DlState)
			{
				ldbg("STATE_GETHEADER");
				header h;
				if (inBuf.size() == 0)
					return HINT_MORE;

				bool bHotItem = (m_DlState == STATE_REGETHEADER);
				dbgline;

				auto hDataLen = h.Load(inBuf.rptr(), inBuf.size(),
						[&h](cmstring& key, cmstring& rest)
						{ if(scaseequals(key, "Content-Location"))
							h.frontLine = "HTTP/1.1 500 Apt-Cacher NG does not like that data";
						});

				if (0 == hDataLen)
					return HINT_MORE;
				if (hDataLen<0)
				{
					dbgline;
					sErrorMsg = "500 Invalid header";
					// can be followed by any junk... drop that mirror, previous file could also contain bad data
					return EFLAG_MIRROR_BROKEN | HINT_DISCON | HINT_KILL_LAST_FILE;
				}

				ldbg("contents: " << std::string(inBuf.rptr(), hDataLen));
				inBuf.drop((unsigned long) hDataLen);
				if (h.type != header::ANSWER)
				{
					dbgline;
					sErrorMsg = "500 Unexpected response type";
					// smells fatal...
					return EFLAG_MIRROR_BROKEN | HINT_DISCON;
				}
				ldbg("GOT, parsed: " << h.frontLine);

				int st = h.getStatus();

				// processing hint 102, or something like 103 which we can ignore
				if(st < 200)
					return HINT_MORE;

				if (cfg::redirmax) // internal redirection might be disabled
				{
					if (IS_REDIRECT(st))
					{
						if (!RewriteSource(h.h[header::LOCATION]))
							return EFLAG_JOB_BROKEN;

						// drop the redirect page contents if possible so the outer loop
						// can scan other headers
						off_t contLen = atoofft(h.h[header::CONTENT_LENGTH], 0);
						if (contLen <= inBuf.size())
							inBuf.drop(contLen);
						return HINT_TGTCHANGE; // no other flags, caller will evaluate the state
					}

					// for non-redirection responses process as usual

					// unless it's a probe run from the outer loop, in this case we
					// should go no further
					if (bOnlyRedirectionActivity)
						return EFLAG_LOST_CON | HINT_DISCON;
				}

				// explicitly blacklist mirror if key file is missing
				if (st >= 400 && m_pRepoDesc && m_remoteUri.sHost.empty())
				{
					for (const auto& kfile : m_pRepoDesc->m_keyfiles)
					{
						if (endsWith(m_remoteUri.sPath, kfile))
						{
							sErrorMsg = "500 Keyfile missing, mirror blacklisted";
							return HINT_DISCON | EFLAG_MIRROR_BROKEN;
						}
					}
				}

				auto pCon = h.h[header::CONNECTION];
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
				else if(st == 304 && cfg::vrangeops == 0)
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
				auto sremote = RemoteUri(false);
				h.set(header::XORIG, sremote);
				bool bDoRetry(false);

				// detect bad auto-redirectors (auth-pages, etc.) by the mime-type of their target
				if(cfg::redirmax
						&& !cfg::badredmime.empty()
						&& cfg::redirmax != m_nRedirRemaining
						&& h.h[header::CONTENT_TYPE]
						&& strstr(h.h[header::CONTENT_TYPE], cfg::badredmime.c_str())
						&& h.getStatus() < 300) // contains the final data/response
				{
					if(m_pStorage->m_bCheckFreshness)
					{
						// volatile... this is still ok, just make sure time check works next time
						h.set(header::LAST_MODIFIED, FAKEDATEMARK);
					}
					else
					{
						// this was redirected and the destination is BAD!
						h.frontLine="HTTP/1.1 501 Redirected to invalid target";
						// XXX: not sure this is the right attribution
						//void DropDnsCache();
						//DropDnsCache();
					}
				}

				if(!m_pStorage->DownloadStartedStoreHeader(h, hDataLen,
						inBuf.rptr(), bHotItem, bDoRetry))
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
				auto res = NewDataHandler(inBuf);
				if (HINT_SWITCH != res)
					return res;
			}
			else if (m_DlState == STATE_FINISHJOB)
			{
				ldbg("STATE_FINISHJOB");
				m_DlState = STATE_GETHEADER;
				m_pStorage->StoreFileData(nullptr, 0);
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
				if (!inBuf.size() || nullptr == (crlf = strstr(pStart, "\r\n")))
				{
					inBuf.move();
					return HINT_MORE;
				}
				unsigned len(0);
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

public:

Impl(cmstring& sOwnersHostname, const IDlConFactory &pConFactory) :
		m_conFactory(pConFactory),
		m_ownersHostname(sOwnersHostname),
		m_nTempPipelineDisable(0),
		m_bProxyTot(false)
{
	LOGSTART("dlcon::Impl::dlcon");
#ifdef HAVE_LINUX_EVENTFD
	m_wakeventfd = eventfd(0, 0);
	if(m_wakeventfd == -1)
		m_ctrl_hint = -1;
	else
		set_nb(m_wakeventfd);
#else
	if (0 == pipe(m_wakepipe))
	{
		set_nb(m_wakepipe[0]);
		set_nb(m_wakepipe[1]);
	}
	else
	{
		m_wakepipe[0] = m_wakepipe[1] = -1;
		m_ctrl_hint = -1;
	}
#endif
  g_nDlCons++;
}

~Impl()
{
	LOGSTART("dlcon::Impl::~dlcon, Destroying dlcon");
#ifdef HAVE_LINUX_EVENTFD
	checkforceclose(m_wakeventfd);
#else
	checkforceclose(m_wakepipe[0]);
	checkforceclose(m_wakepipe[1]);
#endif
  g_nDlCons--;
}

void SignalStop()
{
	LOGSTART("dlcon::Impl::SignalStop");
	// stop all activity as soon as possible
	m_ctrl_hint = -1;
	wake();
}
}; // Impl

dlcon::dlcon(cmstring& sOwnersHostname, const IDlConFactory& pConFactory) : _p(new dlcon::Impl(sOwnersHostname, pConFactory)) {}
dlcon::~dlcon() { delete _p; }
void dlcon::WorkLoop() { return _p->WorkLoop(); }
void dlcon::SignalStop(){ return _p->SignalStop(); }
bool dlcon::AddJob(tFileItemPtr m_pItem, const tHttpUrl *pForcedUrl, const cfg::tRepoData *pRepoDesc, cmstring *sPatSuffix, LPCSTR reqHead,
		int nMaxRedirection, const char *szHeaderXff)
{ return _p->AddJob(m_pItem, pForcedUrl, pRepoDesc, sPatSuffix, reqHead, nMaxRedirection, szHeaderXff); }


#ifdef HAVE_LINUX_EVENTFD
inline void dlcon::Impl::wake()
{
	LOGSTART("dlcon::Impl::wake");
	if(fdWakeWrite == -1)
		return;
	while(true)
	{
		auto r=eventfd_write(fdWakeWrite, 1);
		if(r == 0 || (errno != EINTR && errno != EAGAIN))
			break;
	}

}
inline void dlcon::Impl::awaken_check()
{
	LOGSTART("dlcon::Impl::awaken_check");
	eventfd_t xtmp;
	for(int i=0; ; ++i)
	{
		auto tmp = eventfd_read(fdWakeRead, &xtmp);
		LOG("got one event, res: " << tmp << ", errno: " << errno);
		if(i > 100) // breakpoint... OS bug? what's the errno?
			break;
		if(tmp == -1 && errno == EAGAIN) continue;
		break;
	}
}

#else
void dlcon::Impl::wake()
{
	LOGSTART("dlcon::Impl::wake");
	POKE(fdWakeWrite);
}
inline void dlcon::Impl::awaken_check()
{
	LOGSTART("dlcon::Impl::awaken_check");
	for (char tmp; ::read(m_wakepipe[0], &tmp, 1) > 0;) ;
}

#endif

bool dlcon::Impl::AddJob(tFileItemPtr m_pItem, const tHttpUrl *pForcedUrl,
		const cfg::tRepoData *pBackends,
		cmstring *sPatSuffix, LPCSTR reqHead,
		int nMaxRedirection, const char* szHeaderXff)
{
	if(m_ctrl_hint<0)
		return false;

	if(!pForcedUrl)
	{
		if(!pBackends || pBackends->m_backends.empty())
			return false;
		if(!sPatSuffix || sPatSuffix->empty())
			return false;
	}
	tDlJob xnew(this, m_pItem, pForcedUrl, pBackends, sPatSuffix,nMaxRedirection);
	xnew.ExtractCustomHeaders(reqHead);

	if (cfg::exporigin && !m_ownersHostname.empty())
	{
		if (szHeaderXff)
		{
			xnew.m_xff = szHeaderXff;
			xnew.m_xff+= ", ";
		}
		xnew.m_xff += m_ownersHostname;
	}
	{
		lockguard g(m_handover_mutex);
		m_new_jobs.emplace_back(move(xnew));
	}
	m_ctrl_hint++;

	wake();
	return true;
}

inline unsigned dlcon::Impl::ExchangeData(mstring &sErrorMsg, tDlStreamHandle &con, tDljQueue &inpipe)
{
	LOGSTART2("dlcon::Impl::ExchangeData",
			"qsize: " << inpipe.size() << ", sendbuf size: "
			<< m_sendBuf.size() << ", inbuf size: " << m_inBuf.size());

	fd_set rfds, wfds;
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
		FD_SET(fdWakeRead, &rfds);
		int nMaxFd = fdWakeRead;

		if (fd>=0)
		{
			FD_SET(fd, &rfds);
			nMaxFd = std::max(fd, nMaxFd);

			if (!m_sendBuf.empty())
			{
				ldbg("Needs to send " << m_sendBuf.size() << " bytes");
				FD_SET(fd, &wfds);
			}
#ifdef HAVE_SSL
			else if(con->GetBIO() && BIO_should_write(con->GetBIO()))
			{
				ldbg("NOTE: OpenSSL wants to write although send buffer is empty!");
				FD_SET(fd, &wfds);
			}
#endif
		}

		ldbg("select dlcon");

		// jump right into data processing but only once
		if(bReEntered)
		{
			bReEntered=false;
			goto proc_data;
		}

		r=select(nMaxFd + 1, &rfds, &wfds, nullptr, CTimeVal().ForNetTimeout());
		ldbg("returned: " << r << ", errno: " << errno);
		if(m_ctrl_hint < 0)
			return HINT_DISCON;

		if (r == -1)
		{
			if (EINTR == errno)
				continue;
#ifdef MINIBUILD
			string fer("select failed");
#else
			tErrnoFmter fer("FAILURE: select, ");
			LOG(fer);
#endif
			sErrorMsg = string("500 Internal malfunction, ") + fer;
			return HINT_DISCON|EFLAG_JOB_BROKEN|EFLAG_MIRROR_BROKEN;
		}
		else if (r == 0) // looks like a timeout
		{
			sErrorMsg = "500 Connection timeout";
			LOG(sErrorMsg);

			// was there anything to do at all?
			if(inpipe.empty())
				return HINT_SWITCH;

			if(inpipe.front().IsRecoverableState())
				return EFLAG_LOST_CON;
			else
				return (HINT_DISCON|EFLAG_JOB_BROKEN);
		}

		if (FD_ISSET(fdWakeRead, &rfds))
		{
			awaken_check();
			return HINT_SWITCH;
		}

		if (fd >= 0)
		{
			if (FD_ISSET(fd, &wfds))
			{
				FD_CLR(fd, &wfds);

#ifdef HAVE_SSL
				if (con->GetBIO())
				{
					int s = BIO_write(con->GetBIO(), m_sendBuf.rptr(),
							m_sendBuf.size());
					ldbg(
							"tried to write to SSL, " << m_sendBuf.size() << " bytes, result: " << s);
					if (s > 0)
						m_sendBuf.drop(s);
				}
				else
				{
#endif
					ldbg("Sending data...\n" << m_sendBuf);
					int s = ::send(fd, m_sendBuf.data(), m_sendBuf.length(),
							MSG_NOSIGNAL);
					ldbg(
							"Sent " << s << " bytes from " << m_sendBuf.length() << " to " << con.get());
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
#ifdef HAVE_SSL
			}
#endif
		}

		if (fd >=0 && (FD_ISSET(fd, &rfds)
#ifdef HAVE_SSL
				|| (con->GetBIO() && BIO_should_read(con->GetBIO()))
#endif
			))
		{
       if(cfg::maxdlspeed != cfg::RESERVED_DEFVAL)
       {
          auto nCntNew=g_nDlCons.load();
          if(m_nLastDlCount != nCntNew)
          {
             m_nLastDlCount=nCntNew;

             // well, split the bandwidth
             auto nSpeedNowKib = uint(cfg::maxdlspeed) / nCntNew;
             auto nTakesPerSec = nSpeedNowKib / 32;
             if(!nTakesPerSec)
                nTakesPerSec=1;
             m_nSpeedLimitMaxPerTake = nSpeedNowKib*1024/nTakesPerSec;
             auto nIntervalUS=1000000 / nTakesPerSec;
             auto nIntervalUS_copy = nIntervalUS;
             // creating a bitmask
             for(m_nSpeedLimiterRoundUp=1,nIntervalUS/=2;nIntervalUS;nIntervalUS>>=1)
                m_nSpeedLimiterRoundUp = (m_nSpeedLimiterRoundUp<<1)|1;
             m_nSpeedLimitMaxPerTake = uint(double(m_nSpeedLimitMaxPerTake) * double(m_nSpeedLimiterRoundUp) / double(nIntervalUS_copy));
          }
          // waiting for the next time slice to get data from buffer
          timeval tv;
          if(0==gettimeofday(&tv, nullptr))
          {
             auto usNext = tv.tv_usec | m_nSpeedLimiterRoundUp;
             usleep(usNext-tv.tv_usec);
          }
       }
#ifdef HAVE_SSL
			if(con->GetBIO())
			{
				r=BIO_read(con->GetBIO(), m_inBuf.wptr(),
						std::min(m_nSpeedLimitMaxPerTake, m_inBuf.freecapa()));
				if(r>0)
					m_inBuf.got(r);
				else // <=0 doesn't mean an error, only a double check can tell
					r=BIO_should_read(con->GetBIO()) ? 1 : -errno;
			}
			else
#endif
			{
				r = m_inBuf.sysread(fd, m_nSpeedLimitMaxPerTake);
			}

#ifdef DISCO_FAILURE
#warning hej
			static int fakeFail=-123;
			if(fakeFail == -123)
			{
				srand(getpid());
				fakeFail = rand()%123;
			}
			if( fakeFail-- < 0)
			{
//				LOGLVL(log::LOG_DEBUG, "\n#################\nFAKING A FAILURE\n###########\n");
				r=0;
				fakeFail=rand()%123;
				errno = EROFS;
				//r = -errno;
				shutdown(con.get()->GetFD(), SHUT_RDWR);
			}
#endif

			if(r == -EAGAIN || r == -EWOULDBLOCK)
			{
				ldbg("why EAGAIN/EINTR after getting it from select?");
//				timespec sleeptime = { 0, 432000000 };
//				nanosleep(&sleeptime, nullptr);
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
				sErrorMsg = tErrnoFmter("502 ");
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

				ldbg("Processing job for " << inpipe.front().RemoteUri(false));
				unsigned res = inpipe.front().ProcessIncomming(m_inBuf, false);
				ldbg(
						"... incoming data processing result: " << res
						<< ", emsg: " << inpipe.front().sErrorMsg);

				if(res&EFLAG_MIRROR_BROKEN)
				{
					ldbg("###### BROKEN MIRROR ####### on " << con.get());
				}

				if (HINT_MORE == res)
					goto loop_again;

				if (HINT_DONE & res)
				{
					// just in case that server damaged the last response body
					con->KnowLastFile(WEAK_PTR<fileitem>(inpipe.front().m_pStorage));

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
					auto it = inpipe.begin();
					for(++it; it != inpipe.end(); ++it)
					{
						unsigned rr = it->ProcessIncomming(m_inBuf, true);
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
				setIfNotEmpty(sErrorMsg, inpipe.front().sErrorMsg);
				return res;
			}
			return HINT_DONE; // input buffer consumed
		}
	}

	ASSERT(!"Unreachable");
	sErrorMsg = "500 Internal failure";
	return EFLAG_JOB_BROKEN|HINT_DISCON;
}

void dlcon::Impl::WorkLoop()
{
	LOGSTART("dlcon::Impl::WorkLoop");
    string sErrorMsg;
    m_inBuf.clear();
    
	if (!m_inBuf.setsize(cfg::dlbufsize))
	{
		log::err("500 Out of memory");
		return;
	}

	if(fdWakeRead<0 || fdWakeWrite<0)
	{
		log::err("Error creating pipe file descriptors");
		return;
	}

	tDljQueue next_jobs, active_jobs;

	tDtorEx allJobReleaser([&](){ next_jobs.clear(); active_jobs.clear(); });

	tDlStreamHandle con;
	unsigned loopRes=0;

	bool bStopRequesting=false; // hint to stop adding request headers until the connection is restarted

	int nLostConTolerance=MAX_RETRY;

	auto BlacklistMirror = [&](tDlJob & job)
	{
		LOGSTART2("BlacklistMirror", "blacklisting " <<
				job.GetPeerHost().ToURI(false));
		m_blacklist[std::make_pair(job.GetPeerHost().sHost,
				job.GetPeerHost().GetPort())] = sErrorMsg;
	};

	auto prefProxy = [&](tDlJob& cjob) -> const tHttpUrl*
	{
		if(m_bProxyTot)
			return nullptr;

		if(cjob.m_pRepoDesc && cjob.m_pRepoDesc->m_pProxy
				&& !cjob.m_pRepoDesc->m_pProxy->sHost.empty())
		{
			return cjob.m_pRepoDesc->m_pProxy;
		}
		return cfg::GetProxyInfo();
	};
	int lastCtrlMark = -2;
	while(true) // outer loop: jobs, connection handling
	{
		// init state or transfer loop jumped out, what are the needed actions?
		LOG("New next_jobs: " << next_jobs.size());

		if(m_ctrl_hint < 0) // check for ordered shutdown
		{
			/* The no-more-users checking logic will purge orphaned items from the inpipe
			 * queue. When the connection is dirty after that, it will be closed in the
			 * ExchangeData() but if not then it can be assumed to be clean and reusable.
			 */
			if(active_jobs.empty())
			{
				if(con)
					m_conFactory.RecycleIdleConnection(con);
				return;
			}
		}
		int newCtrlMark = m_ctrl_hint;
		if(newCtrlMark != lastCtrlMark)
		{
			lastCtrlMark = newCtrlMark;
			lockguard g(m_handover_mutex);
			next_jobs.splice(next_jobs.begin(), m_new_jobs);
		}

		if(next_jobs.empty() && active_jobs.empty())
			goto go_select; // parent will notify RSN

		if(!con)
		{
			// cleanup after the last connection - send buffer, broken next_jobs, ...
			m_sendBuf.clear();
			m_inBuf.clear();
			active_jobs.clear();

			bStopRequesting=false;

			for(tDljQueue::iterator it=next_jobs.begin(); it!=next_jobs.end();)
			{
				if(it->SetupJobConfig(sErrorMsg, m_blacklist))
					++it;
				else
				{
					setIfNotEmpty2( it->sErrorMsg, sErrorMsg,
							"500 Broken mirror or incorrect configuration");
					it = next_jobs.erase(it);
				}
			}
			if(next_jobs.empty())
			{
				LOG("no next_jobs left, start waiting")
				goto go_select; // nothing left, might receive new next_jobs soon
			}

			bool bUsed = false;
			ASSERT(!next_jobs.empty());
			auto doconnect = [&](const tHttpUrl& tgt, int timeout, bool fresh)
			{
				return m_conFactory.CreateConnected(tgt.sHost,
						tgt.GetPort(),
						sErrorMsg,
						&bUsed,
						next_jobs.front().GetConnStateTracker(),
						IFSSLORFALSE(tgt.bSSL),
						timeout, fresh);
		}	;

			auto& cjob = next_jobs.front();
			auto proxy = prefProxy(cjob);
			auto& peerHost = cjob.GetPeerHost();

#ifdef HAVE_SSL
			if(peerHost.bSSL)
			{
				if(proxy)
				{
					con = doconnect(*proxy, cfg::optproxytimeout > 0 ?
							cfg::optproxytimeout : cfg::nettimeout, false);
					if(con)
					{
						if(!con->StartTunnel(peerHost, sErrorMsg, & proxy->sUserPass, true))
							con.reset();
					}
				}
				else
					con = doconnect(peerHost, cfg::nettimeout, false);
			}
			else
#endif
			{
				if(proxy)
				{
					con = doconnect(*proxy, cfg::optproxytimeout > 0 ?
							cfg::optproxytimeout : cfg::nettimeout, false);
				}
				else
					con = doconnect(peerHost, cfg::nettimeout, false);
			}

			if(!con && proxy && cfg::optproxytimeout>0)
			{
				ldbg("optional proxy broken, disable");
				m_bProxyTot = true;
				proxy = nullptr;
			cfg::MarkProxyFailure();
				con = doconnect(peerHost, cfg::nettimeout, false);
			}

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
				BlacklistMirror(cjob);
				continue; // try the next backend
			}
		}

		// connection should be stable now, prepare all jobs and/or move to pipeline
		while(!bStopRequesting
				&& !next_jobs.empty()
				&& int(active_jobs.size()) <= cfg::pipelinelen)
		{
			auto &frontJob = next_jobs.front();

			if(!frontJob.SetupJobConfig(sErrorMsg, m_blacklist))
			{
				// something weird happened to it, drop it and let the client care
				next_jobs.pop_front();
				continue;
			}

			auto& tgt=frontJob.GetPeerHost();
			// good case, direct or tunneled connection
			bool match=(tgt.sHost == con->GetHostname() && tgt.GetPort() == con->GetPort());
			const tHttpUrl * proxy = nullptr; // to be set ONLY if PROXY mode is used

			// if not exact and can be proxied, and is this the right proxy?
			if(!match)
			{
				proxy = prefProxy(frontJob);
				if(proxy)
				{
					/*
					 * SSL over proxy uses HTTP tunnels (CONNECT scheme) so the check
					 * above should have matched before.
					 */
					if(!tgt.bSSL)
						match=(proxy->sHost == con->GetHostname() && proxy->GetPort() == con->GetPort());
				}
				// else... host changed and not going through the same proxy -> fail
			}

			if(!match)
			{
				LOG("host mismatch, new target: " << tgt.sHost << ":" << tgt.GetPort());
				bStopRequesting=true;
				break;
			}

			frontJob.AppendRequest(m_sendBuf, proxy);
			LOG("request added to buffer");
			auto itSecond = next_jobs.begin();
			active_jobs.splice(active_jobs.end(), next_jobs, next_jobs.begin(), ++itSecond);

			if (m_nTempPipelineDisable > 0)
			{
				bStopRequesting = true;
				--m_nTempPipelineDisable;
				break;
			}
		}


		ldbg("Request(s) cooked, buffer contents: " << m_sendBuf);

        go_select:

        // inner loop: plain communication until something happens. Maybe should use epoll here?
        loopRes=ExchangeData(sErrorMsg, con, active_jobs);
        ldbg("loopRes: "<< loopRes);
        if(m_ctrl_hint < 0)
        	return;

        /* check whether we have a pipeline stall. This may happen because a) we are done or
         * b) because of the remote hostname change or c) the client stopped sending tasks.
         * Anyhow, that's a reason to put the connection back into the shared pool so either we
         * get it back from the pool in the next workloop cycle or someone else gets it and we
         * get a new connection for the new host later.
         * */
        if (active_jobs.empty())
		{
        	// all requests have been processed (client done, or pipeline stall, who cares)
			dbgline;

			// no matter what happened, that stop flag is now irrelevant
			bStopRequesting = false;

			// no error bits set, not busy -> this connection is still good, recycle properly
			unsigned all_err = HINT_DISCON | EFLAG_JOB_BROKEN | EFLAG_LOST_CON | EFLAG_MIRROR_BROKEN;
			if (con && !(loopRes & all_err))
			{
				dbgline;
				m_conFactory.RecycleIdleConnection(con);
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
        	// short queue continues next_jobs with rewritten targets, so
        	// reinsert them into the new task list and continue

        	// if conn was not reset above then it should be in good shape
        	m_conFactory.RecycleIdleConnection(con);
        	goto move_jobs_back_to_q;
        }

        if ((EFLAG_LOST_CON & loopRes) && !active_jobs.empty())
		{
			// disconnected by OS... give it a chance, or maybe not...
			if (--nLostConTolerance <= 0)
			{
				BlacklistMirror(active_jobs.front());
				nLostConTolerance=MAX_RETRY;
			}

			con.reset();

			timespec sleeptime = { 0, 325000000 };
			nanosleep(&sleeptime, nullptr);

			// trying to resume that job secretly, unless user disabled the use of range (we
			// cannot resync the sending position ATM, throwing errors to user for now)
			if (cfg::vrangeops <= 0 && active_jobs.front().m_pStorage->m_bCheckFreshness)
				loopRes |= EFLAG_JOB_BROKEN;
			else
				active_jobs.front().m_DlState = tDlJob::STATE_REGETHEADER;
		}

        if(loopRes & (HINT_DONE|HINT_MORE))
        {
        	sErrorMsg.clear();
        	continue;
        }

        //
        // regular required post-processing done here, now handle special conditions
        //


        if(HINT_SWITCH == loopRes)
        	continue;

        // resolving the "fatal error" situation, push the pipelined job back to new, etc.

        if( (EFLAG_MIRROR_BROKEN & loopRes) && !active_jobs.empty())
        	BlacklistMirror(active_jobs.front());

        if( (EFLAG_JOB_BROKEN & loopRes) && !active_jobs.empty())
        {
        	setIfNotEmpty(active_jobs.front().sErrorMsg, sErrorMsg);

        	active_jobs.pop_front();

        	if(EFLAG_STORE_COLLISION & loopRes)
        	{
				// stupid situation, both users downloading the same stuff - and most likely in the same order
				// if one downloader runs a step ahead (or usually many steps), drop all items
				// already processed by it and try to continue somewhere else.
				// This way, the overall number of collisions and reconnects is minimized

        		for(auto pJobList : {&active_jobs, &next_jobs})
        		{
        			auto& joblist(*pJobList);
        			for(auto it = joblist.begin(); it!= joblist.end();)
        			{
        				// someone else is doing it -> drop
        				if(it->m_pStorage && it->m_pStorage->GetStatus() >= fileitem::FIST_DLRECEIVING)
        					it = joblist.erase(it);
        				else
        					++it;
        			}
        		};
        	}
        }

        move_jobs_back_to_q:
       	next_jobs.splice(next_jobs.begin(), active_jobs);
	}
}

}
