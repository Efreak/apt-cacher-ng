//#define LOCAL_DEBUG
#include "debug.h"

#include "job.h"
#include <cstdio>
#include <stdexcept>
#include <limits>
#include <queue>
using namespace std;

#include "conn.h"
#include "acfg.h"
#include "fileitem.h"
#include "dlcon.h"
#include "sockio.h"
#include "fileio.h" // for ::stat and related macros
#include <dirent.h>
#include <algorithm>
#include "maintenance.h"

#include <errno.h>

//#define TESTCHUNKMODE
#ifdef TESTCHUNKMODE
#warning testing chunk mode
#endif

namespace acng
{

mstring sHttp11("HTTP/1.1");

#define SPECIAL_FD -42
inline bool IsValidFD(int fd)
{
	return fd >= 0 || SPECIAL_FD == fd;
}

tTraceData traceData;
void cfg::dump_trace()
{
	log::err("Paths with uncertain content types:");
	lockguard g(traceData);
	for (const auto& s : traceData)
		log::err(s);
}
tTraceData& tTraceData::getInstance()
{
	return traceData;
}

/*
 * Unlike the regular store-and-forward file item handler, this ones does not store anything to
 * harddisk. Instead, it uses the download buffer and lets job object send the data straight from
 * it to the client.
 */
class tPassThroughFitem: public tFileItemEx
{
protected:

	// forward buffer, remaining part, cursor position
	const char *m_pData = 0;
	size_t m_nConsumable = 0, m_nConsumed = 0;
	conn& m_parent;

public:
	tPassThroughFitem(std::string s, conn& par) :
		tFileItemEx(s),
			m_parent(par)
	{
		m_bAllowStoreData = false;
		m_nCheckedSize = m_nSizeSeenInCache = 0;
	}
	;
	virtual FiStatus SetupFromCache(bool) override
	{
		m_nCheckedSize = m_nSizeSeenInCache = 0;
		return m_status = FIST_INITED;
	}
	virtual int GetFileFd() override
	{
		return SPECIAL_FD;
	}
	; // something, don't care for now
	virtual bool DownloadStartedStoreHeader(const header & h, size_t hDataLen,
			const char *pNextData,
			bool bRestartResume, bool &bDoCleanRetry) override
	{
		// behave like normal item but forbid data modifying operations
		m_bAllowStoreData = false;
		return tFileItemEx::DownloadStartedStoreHeader(h, hDataLen, pNextData, bRestartResume, bDoCleanRetry);
	}
	virtual bool StoreFileData(const char *data, unsigned int size) override
	{
		LOGSTART2("tPassThroughFitem::StoreFileData", "status: " << (int) m_status);

		m_nIncommingCount += size;

		dbgline;
		if (m_status > fileitem::FIST_COMPLETE || m_status < FIST_DLGOTHEAD)
			return false;

		if (size == 0)
			m_status = FIST_COMPLETE;
		else
		{
			dbgline;
			m_status = FIST_DLRECEIVING;
			m_nCheckedSize += size;
			m_pData = data;
			m_nConsumable = size;
			m_nConsumed = 0;

#warning fixme
//			m_parent.dlPaused = true;

			// let the downloader abort?
			if (m_status >= FIST_DLERROR)
				return false;
		}
		return true;
	}
	ssize_t SendData(int out_fd, int, off_t &nSendPos, size_t nMax2SendNow) override
	{
		lockguard g(m_mx);

		while (0 == m_nConsumable && m_status <= FIST_COMPLETE
				&& !(m_nCheckedSize == 0 && m_status == FIST_COMPLETE))
		{
			return 0; // will get more or block until then
		}
		if (m_status >= FIST_DLERROR || !m_pData)
			return -1;

		if (!m_nCheckedSize)
			return 0;

		auto r = write(out_fd, m_pData, min(nMax2SendNow, m_nConsumable));
		if (r < 0 && (errno == EAGAIN || errno == EINTR)) // harmless
			r = 0;
		if (r < 0)
		{
			m_status = FIST_DLERROR;
			m_head.frontLine = "HTTP/1.1 500 Data passing error";
#warning fixme
//			m_parent.dlPaused = false; // needs to shutdown
		}
		else if (r > 0)
		{
			m_nConsumable -= r;
			m_pData += r;
			m_nConsumed += r;
			nSendPos += r;
#warning fixme
//			if (m_nConsumable <= 0)
//				m_parent.dlPaused = false;
		}
		return r;
	}
};

// base class for a fileitem replacement with custom storage for local data
class tGeneratedFitemBase: public fileitem
{
public:
	virtual int GetFileFd() override
	{
		return SPECIAL_FD;
	}
	; // something, don't care for now

	tSS m_data;

	tGeneratedFitemBase(const string &sFitemId, cmstring &sFrontLineMsg) :
			m_data(256)
	{
		m_status = FIST_COMPLETE;
		m_sPathRel = sFitemId;
		m_head.type = header::ANSWER;
		m_head.frontLine = "HTTP/1.1 ";
		m_head.frontLine +=
				sFrontLineMsg.empty() ? cmstring("500 Internal Failure") : sFrontLineMsg;
		m_head.set(header::CONTENT_TYPE, WITHLEN("text/html"));
	}
	ssize_t SendData(int out_fd, int, off_t &nSendPos, size_t nMax2SendNow) override
	{
		if (m_status > FIST_COMPLETE || out_fd < 0)
			return -1;
		auto r = m_data.syswrite(out_fd, nMax2SendNow);
		if (r > 0)
			nSendPos += r;
		return r;
	}
	inline void seal()
	{
		// seal the item
		m_nCheckedSize = m_data.size();
		m_head.set(header::CONTENT_LENGTH, m_nCheckedSize);
	}
	// never used to store data
	bool DownloadStartedStoreHeader(const header &, size_t, const char *, bool, bool&) override
	{
		return false;
	}
	;
	bool StoreFileData(const char *, unsigned int) override
	{
		return false;
	}
	;
	header & HeadRef()
	{
		return m_head;
	}
};

job::job(header &h, conn &pParent) :
		m_parent(pParent)
{
	LOGSTART2("job::job",
			"job creating, " << h.frontLine << " and this: " << uintptr_t(this));
	m_reqHead.swap(h);
}

static const string miscError(" [HTTP error, code: ");

job::~job()
{
	LOGSTART("job::~job");
	int stcode = 555;
	if (m_pItem)
	{
#warning optimize
		stcode = m_pItem->GetHeaderLocking().getStatus();
	}

	bool bErr = m_sFileLoc.empty() || stcode >= 400;

#warning check logs, counts, etc.
	m_parent.LogDataCounts(m_sFileLoc + (bErr ? (miscError + ltos(stcode) + ']') : sEmptyString),
			m_reqHead.h[header::XFORWARDEDFOR],
			(m_pItem ? m_pItem->m_nIncommingCount : 0), m_parent.j_nRetDataCount, bErr);

#warning when volatile type and finished download, donate this to a quick life-keeping cache for 33s, see cleaner
	// fileitem_with_storage::ProlongLife(m_pItem);
	checkforceclose(m_parent.j_filefd);

	if (m_bFitemWasSubscribed)
		m_parent.UnsubscribeFromUpdates(m_pItem);

}

inline void job::SetupFileServing(const string &visPath, const string &fsBase,
		const string &fsSubpath)
{
	mstring absPath = fsBase + SZPATHSEP + fsSubpath;
	Cstat stbuf(absPath);
	if (!stbuf)
	{
		switch (errno)
		{
		case EACCES:
			return SetFatalErrorResponse("403 Permission denied");
		case ELOOP:
			return SetFatalErrorResponse("500 Infinite link recursion");
		case ENAMETOOLONG:
			return SetFatalErrorResponse("500 File name too long");
		case ENOENT:
		case ENOTDIR:
			return SetFatalErrorResponse("404 File or directory not found");
		case EBADF:
		case EFAULT:
		case ENOMEM:
		case EOVERFLOW:
		default:
			return SetFatalErrorResponse("500 Internal server error");
		}
		return;
	}
	/*
	 // simplified version, just converts a string into a page
	 class bufferitem : public tPassThroughFitem, private string
	 {
	 public:
	 bufferitem(const string &sId, const string &sData)
	 : tPassThroughFitem(sId), string(sData)
	 {
	 status = FIST_COMPLETE;
	 m_pData=c_str();
	 m_nConsumable=length();
	 m_nSizeChecked=m_nConsumable;
	 m_head.frontLine.assign(_SZ2PS("HTTP/1.1 200 OK"));
	 m_head.set(header::CONTENT_LENGTH, length());
	 m_head.set(header::CONTENT_TYPE, _SZ2PS("text/html") );
	 m_head.type=header::ANSWER;
	 }
	 };
	 */

	if (S_ISDIR(stbuf.st_mode))
	{
		// unconfuse the browser
		if (!endsWithSzAr(visPath, SZPATHSEPUNIX))
		{
			class dirredirect: public tGeneratedFitemBase
			{
			public:
				dirredirect(const string &visPath) :
						tGeneratedFitemBase(visPath, "301 Moved Permanently")
				{
					m_head.set(header::LOCATION, visPath + "/");
					m_data
							<< "<!DOCTYPE html>\n<html lang=\"en\"><head><title>301 Moved Permanently</title></head><body><h1>Moved Permanently</h1>"
									"<p>The document has moved <a href=\"" + visPath
									+ "/\">here</a>.</p></body></html>";

					seal();
				}
			};
			m_pItem.reset(new dirredirect(visPath));
			return;
		}

		class listing: public tGeneratedFitemBase
		{
		public:
			listing(const string &visPath) :
					tGeneratedFitemBase(visPath, "200 OK")
			{
				seal(); // for now...
			}
		};
		auto p = new listing(visPath);
		m_pItem.reset(p);
		tSS & page = p->m_data;

		page << "<!DOCTYPE html>\n<html lang=\"en\"><head><title>Index of " << visPath
				<< "</title></head>"
						"<body><h1>Index of " << visPath
				<< "</h1>"
						"<table><tr><th>&nbsp;</th><th>Name</th><th>Last modified</th><th>Size</th></tr>"
						"<tr><th colspan=\"4\"><hr></th></tr>";

		DIR *dir = opendir(absPath.c_str());
		if (!dir) // weird, whatever... ignore...
			page << "ERROR READING DIRECTORY";
		else
		{
			// quick hack with sorting by custom keys, good enough here
			priority_queue<tStrPair, std::vector<tStrPair>, std::greater<tStrPair>> sortHeap;
			for (struct dirent *pdp(0); 0 != (pdp = readdir(dir));)
			{
				if (0 != ::stat(mstring(absPath + SZPATHSEP + pdp->d_name).c_str(), &stbuf))
					continue;

				bool bDir = S_ISDIR(stbuf.st_mode);

				char datestr[32] =
				{ 0 };
				struct tm tmtimebuf;
				strftime(datestr, sizeof(datestr) - 1, "%d-%b-%Y %H:%M",
						localtime_r(&stbuf.st_mtime, &tmtimebuf));

				string line;
				if (bDir)
					line += "[DIR]";
				else if (startsWithSz(cfg::GetMimeType(pdp->d_name), "image/"))
					line += "[IMG]";
				else
					line += "[&nbsp;&nbsp;&nbsp;]";
				line += string("</td><td><a href=\"") + pdp->d_name + (bDir ? "/\">" : "\">")
						+ pdp->d_name + "</a></td><td>" + datestr + "</td><td align=\"right\">"
						+ (bDir ? string("-") : offttosH(stbuf.st_size));
				sortHeap.push(make_pair(string(bDir ? "a" : "b") + pdp->d_name, line));
				//dbgprint((mstring)line);
			}
			closedir(dir);
			while (!sortHeap.empty())
			{
				page.add(WITHLEN("<tr><td valign=\"top\">"));
				page << sortHeap.top().second;
				page.add(WITHLEN("</td></tr>\r\n"));
				sortHeap.pop();
			}

		}
		page << "<tr><td colspan=\"4\">" << GetFooter();
		page << "</td></tr></table></body></html>";
		p->seal();
		return;
	}
	if (!S_ISREG(stbuf.st_mode))
		return SetFatalErrorResponse("403 Unsupported data type");
	/*
	 * This variant of file item handler sends a local file. The
	 * header data is generated as needed, the relative cache path variable
	 * is reused for the real path.
	 */
	class tLocalGetFitem: public tFileItemEx
	{
	public:
		tLocalGetFitem(string sLocalPath, struct stat &stdata) :
				tFileItemEx(sLocalPath)
		{
			m_bAllowStoreData = false;
			m_status = FIST_COMPLETE;
			m_nCheckedSize = m_nSizeSeenInCache = stdata.st_size;
			m_bCheckFreshness = false;
			m_head.type = header::ANSWER;
			m_head.frontLine = "HTTP/1.1 200 OK";
			m_head.set(header::CONTENT_LENGTH, stdata.st_size);
			m_head.prep(header::LAST_MODIFIED, 26);
			if (m_head.h[header::LAST_MODIFIED])
				FormatTime(m_head.h[header::LAST_MODIFIED], 26, stdata.st_mtim.tv_sec);
			cmstring &sMimeType = cfg::GetMimeType(sLocalPath);
			if (!sMimeType.empty())
				m_head.set(header::CONTENT_TYPE, sMimeType);
		}
		;
		virtual int GetFileFd() override
		{
			int fd = open(m_sPathRel.c_str(), O_RDONLY);
#ifdef HAVE_FADVISE
			// optional, experimental
			if (fd >= 0)
				posix_fadvise(fd, 0, m_nCheckedSize, POSIX_FADV_SEQUENTIAL);
#endif
			return fd;
		}
	};
	m_pItem.reset(new tLocalGetFitem(absPath, stbuf));
}

inline bool job::ParseRange()
{
	/*
	 * Range: bytes=453291-
	 * ...
	 * Content-Length: 7271829
	 * Content-Range: bytes 453291-7725119/7725120
	 */

	const char *pRange = m_reqHead.h[header::RANGE];
	// working around a bug in old curl versions
	if (!pRange)
		pRange = m_reqHead.h[header::CONTENT_RANGE];
	if (pRange)
	{
		int nRangeItems = sscanf(pRange, "bytes=" OFF_T_FMT
		"-" OFF_T_FMT, &m_nReqRangeFrom, &m_nReqRangeTo);
		// working around bad (old curl style) requests
		if (nRangeItems <= 0)
		{
			nRangeItems = sscanf(pRange, "bytes "
			OFF_T_FMT "-" OFF_T_FMT, &m_nReqRangeFrom, &m_nReqRangeTo);
		}

		if (nRangeItems < 1) // weird...
			m_nReqRangeFrom = m_nReqRangeTo = -2;
		else
			return true;
	}
	return false;
}

tSS& job::PrepareErrorResponse()
{
	m_bChunkMode = false;
	m_bClientWants2Close = true;
	tSplitWalk tokenizer(&m_reqHead.frontLine, SPACECHARS);
	trimBack(m_reqHead.frontLine);
	m_bIsHttp11 = endsWith(m_reqHead.frontLine, sHttp11);
	m_stateExternal = XSTATE_CAN_SEND;
	m_stateInternal = STATE_SEND_BUFFER;
	m_stateBackSend = STATE_FINISHJOB;
	return m_parent.j_sendbuf;

}

void job::PrepareDownload(LPCSTR headBuf)
{

	LOGSTART("job::PrepareDownload");

#ifdef DEBUGLOCAL
	cfg::localdirs["stuff"]="/tmp/stuff";
	log::err(m_reqHead.ToString());
#endif

// return with impure call is better than goto
#define msg_overload SetFatalErrorResponse("503 Server overload, please try later")
#define msg_offline SetFatalErrorResponse("503 Unable to download in offline mode")
#define msg_invpath  SetFatalErrorResponse("403 Invalid path specification")
#define msg_degraded  SetFatalErrorResponse("403 Cache server in degraded mode")
#define msg_invport  SetFatalErrorResponse("403 Configuration error (confusing proxy mode) or prohibited port (see AllowUserPorts)")
#define msg_notallowed SetFatalErrorResponse("403 Forbidden file type or location")

	auto _SwitchToPtItem = [this]() ->fileitem::FiStatus
	{
		// Changing to local pass-through file item
			LOGSTART("job::_SwitchToPtItem");
			m_pItem.reset(new tPassThroughFitem(m_sFileLoc, m_parent));
			m_pItem->m_nSizeSeenInCache = m_nReqRangeFrom;
			m_pItem->m_nRangeLimit = m_nReqRangeTo;
			auto ifmo =	m_reqHead.h[header::IF_MODIFIED_SINCE] ?
					m_reqHead.h[header::IF_MODIFIED_SINCE] : m_reqHead.h[header::IFRANGE];
			m_pItem->m_head.set(header::LAST_MODIFIED, ifmo);
			m_pItem->m_bCheckFreshness = ifmo;
			return (m_pItem->m_status = fileitem::FIST_INITED);
		};

	string sReqPath, sPathResidual;
	tHttpUrl theUrl; // parsed URL

	// resolve to an internal repo location and maybe backends later
	cfg::tRepoResolvResult repoMapping;

	fileitem::FiStatus fistate(fileitem::FIST_FRESH);
	bool bForceFreshnessChecks(false); // force update of the file, i.e. on dynamic index files?
	tSplitWalk tokenizer(&m_reqHead.frontLine, SPACECHARS);

	if (m_reqHead.type != header::GET && m_reqHead.type != header::HEAD)
		return msg_invpath;
	if (!tokenizer.Next() || !tokenizer.Next()) // at path...
		return msg_invpath;
	UrlUnescapeAppend(tokenizer, sReqPath);
	if (!tokenizer.Next()) // at proto
		return msg_invpath;

	m_bIsHttp11 = (sHttp11 == tokenizer.str());

	USRDBG("Decoded request URI: " << sReqPath);

	// a sane default? normally, close-mode for http 1.0, keep for 1.1.
	// But if set by the client then just comply!
	m_bClientWants2Close = !m_bIsHttp11;
	if (m_reqHead.h[header::CONNECTION])
		m_bClientWants2Close = !strncasecmp(m_reqHead.h[header::CONNECTION], "close", 5);

	// "clever" file system browsing attempt?
	if (rex::Match(sReqPath, rex::NASTY_PATH) || stmiss != sReqPath.find(MAKE_PTR_0_LEN("/_actmp"))
	|| startsWithSz(sReqPath, "/_"))
		return msg_notallowed;

	try
	{
		if (startsWithSz(sReqPath, "/HTTPS///"))
			sReqPath.replace(0, 6, PROT_PFX_HTTPS);
		// special case: proxy-mode AND special prefix are there
		if (0 == strncasecmp(sReqPath.c_str(), WITHLEN("http://https///")))
			sReqPath.replace(0, 13, PROT_PFX_HTTPS);

		if (!theUrl.SetHttpUrl(sReqPath, false))
		{
			m_eMaintWorkType = tSpecialRequest::workUSERINFO;
			return;
		}
		LOG("refined path: " << theUrl.sPath << "\n on host: " << theUrl.sHost);

		// extract the actual port from the URL
		char *pEnd(0);
		unsigned nPort = 80;
		LPCSTR sPort = theUrl.GetPort().c_str();
		if (!*sPort)
		{
			nPort = (uint) strtoul(sPort, &pEnd, 10);
			if ('\0' != *pEnd || pEnd == sPort || nPort > TCP_PORT_MAX || !nPort)
				return msg_invport;
		}

		if (cfg::pUserPorts)
		{
			if (!cfg::pUserPorts->test(nPort))
				return msg_invport;
		}
		else if (nPort != 80)
			return msg_invport;

		// kill multiple slashes
		for (tStrPos pos = 0; stmiss != (pos = theUrl.sPath.find("//", pos, 2));)
			theUrl.sPath.erase(pos, 1);

		{
			auto bPtMode = rex::MatchUncacheable(theUrl.ToURI(false), rex::NOCACHE_REQ);

			LOG(
					"input uri: "<<theUrl.ToURI(false)<<" , dontcache-flag? " << bPtMode << ", admin-page: " << cfg::reportpage);

			if (bPtMode)
				goto setup_pt_request;
		}

		using namespace rex;
		{
			if ((!cfg::reportpage.empty() || theUrl.sHost == "style.css"))
			{
				m_eMaintWorkType = tSpecialRequest::DispatchMaintWork(sReqPath,
						m_reqHead.h[header::AUTHORIZATION]);
				if (m_eMaintWorkType != tSpecialRequest::workNotSpecial)
				{
					m_sFileLoc = sReqPath;
					return;
				}
			}

			tStrMap::const_iterator it = cfg::localdirs.find(theUrl.sHost);
			if (it != cfg::localdirs.end())
			{
				SetupFileServing(sReqPath, it->second, theUrl.sPath);
				ParseRange();
				return;
			}

			// entered directory but not defined as local? Then 404 it with hints
			if (!theUrl.sPath.empty() && endsWithSzAr(theUrl.sPath, "/"))
			{
				LOG("generic user information page for " << theUrl.sPath);
				m_eMaintWorkType = tSpecialRequest::workUSERINFO;
				return;
			}

			m_type = GetFiletype(theUrl.sPath);

			if (m_type == FILE_INVALID)
			{
				if (!cfg::patrace)
					return msg_notallowed;

				// ok, collect some information helpful to the user
				m_type = FILE_VOLATILE;
				/*				lockguard g(traceData);
				 traceData.insert(theUrl.sPath);

				 */
			}
		}

		// got something valid, has type now, trace it
		USRDBG("Processing new job, "<<m_reqHead.frontLine);

		cfg::GetRepNameAndPathResidual(theUrl, repoMapping);
		if (repoMapping.psRepoName && !repoMapping.psRepoName->empty())
			m_sFileLoc = *repoMapping.psRepoName + SZPATHSEP + repoMapping.sRestPath;
		else
			m_sFileLoc = theUrl.sHost + theUrl.sPath;

		bForceFreshnessChecks = (!cfg::offlinemode && m_type == FILE_VOLATILE);

		m_pItem = tFileItemEx::CreateRegistered(m_sFileLoc);
	} catch (std::out_of_range&) // better safe...
	{
		return msg_invpath;
	}

	if (!m_pItem)
	{
		USRDBG("Error creating file item for " << m_sFileLoc);
		return msg_overload;
	}

	if (cfg::DegradedMode())
		return msg_degraded;

	fistate = m_pItem->SetupFromCache(bForceFreshnessChecks);
	LOG("Got initial file status: " << (int) fistate);

	ParseRange();

	// might need to update the filestamp because nothing else would trigger it
	if (cfg::trackfileuse && fistate >= fileitem::FIST_DLGOTHEAD
			&& fistate < fileitem::FIST_DLERROR)
		m_pItem->UpdateHeadTimestamp();

	if (fistate == fileitem::FIST_COMPLETE)
		return; // perfect, done here

	// early optimization for requests which want a defined file part which we already have
	// no matter whether the file is complete or not
	// handles different cases: GET with range, HEAD with range, HEAD with no range
	if ((m_nReqRangeFrom >= 0 && m_nReqRangeTo >= 0)
			|| (m_reqHead.type == header::HEAD && 0 != (m_nReqRangeTo = -1)))
	{
		if (m_pItem->m_nCheckedSize >= m_nReqRangeTo)
		{
			LOG("Got a partial request for incomplete download but sufficient range is available.");
			m_eDlType = DLSTATE_NOTNEEDED;
			m_stateExternal = XSTATE_CAN_SEND;
			m_stateInternal = STATE_SEND_MAIN_HEAD;
			return;
		}
	}

	if (cfg::offlinemode)
	{ // make sure there will be no problems later in SendData or prepare a user message
	  // error or needs download but freshness check was disabled, so it's really not complete.
		return msg_offline;
	}
	dbgline;
	if (fistate < fileitem::FIST_DLASSIGNED) // needs a downloader
	{
		dbgline;
		if (!m_parent.SetupDownloader(m_reqHead.h[header::XFORWARDEDFOR]))
		{
			USRDBG("Error creating download handler for "<<m_sFileLoc);
			return msg_overload;
		}

		dbgline;
		try
		{
			auto bHaveRedirects =
					(repoMapping.repodata && !repoMapping.repodata->m_backends.empty());

			if (cfg::forcemanaged && !bHaveRedirects)
				return msg_notallowed;

			// XXX: this only checks the first found backend server, what about others?
			auto testUri =
					bHaveRedirects ?
							repoMapping.repodata->m_backends.front().ToURI(false)
									+ repoMapping.sRestPath :
							theUrl.ToURI(false);
			if (rex::MatchUncacheable(testUri, rex::NOCACHE_TGT))
				fistate = _SwitchToPtItem();

			if (m_parent.m_pDlClient->AddJob(m_pItem, bHaveRedirects ? nullptr : &theUrl,
					repoMapping.repodata, bHaveRedirects ? &repoMapping.sRestPath : nullptr,
					nullptr, cfg::redirmax))
			{
				ldbg("Download job enqueued for " << m_sFileLoc);
#warning fixme
//			m_parent.m_lastDlState.flags |= dlcon::tWorkState::needConnect;
			}
			else
			{
				ldbg("PANIC! Error creating download job for " << m_sFileLoc);
				return msg_overload;
			}
		} catch (std::bad_alloc&) // OOM, may this ever happen here?
		{
			USRDBG("Out of memory");
			return msg_overload;
		};
	}

	return;

	setup_pt_request:
	{
		m_sFileLoc = theUrl.ToURI(false);
		ParseRange();
		_SwitchToPtItem();
		if (!m_parent.SetupDownloader(m_reqHead.h[header::XFORWARDEDFOR]))
		{
			USRDBG("Error creating download handler for "<<m_sFileLoc);
			return msg_overload;
		}
		if (m_parent.m_pDlClient->AddJob(m_pItem, &theUrl, repoMapping.repodata, nullptr, headBuf,
				cfg::redirmax))
		{
			ldbg("Download job enqueued for " << m_sFileLoc);
#warning fixme
//			m_parent.m_lastDlState.flags |= dlcon::tWorkState::needConnect;
		}
		else
		{
			ldbg("PANIC! Error creating download job for " << m_sFileLoc);
			return msg_overload;
		}
	}
}

#ifdef DEBUG
#define THROW_ERROR(x) { SetFatalErrorResponse(mstring(x) + ", errno: " + ltos(errno) + " / " + tErrnoFmter()   ); continue; }
#else
#define THROW_ERROR(x) { SetFatalErrorResponse(x); continue; }
#endif

void job::SendData(int confd)
{
	LOGSTART("job::SendData");

	if(m_stateInternal != STATE_FATAL_ERROR)
	{
		if (m_eMaintWorkType)
		{
			tSpecialRequest::RunMaintWork(m_eMaintWorkType, m_sFileLoc, confd);
			m_stateExternal = XSTATE_DISCON;

			return;
		}

		if (!m_pItem)
		{
			m_stateExternal = XSTATE_DISCON;
			return;
		}
	}

	fileitem::FiStatus fistate = fileitem::FIST_FRESH;

	// can avoid data checks when switching around, but only once
	bool skipFetchState = false;

	// eof?
#define GOTOENDE { m_stateInternal = STATE_FINISHJOB ; skipFetchState = true; continue; }

	while (true) // send initial header, chunk header, etc. in a sequence; eventually left by returning
	{
		if (m_stateExternal == XSTATE_DISCON)
			return;

		if(m_stateInternal != STATE_FATAL_ERROR)
		{
			if (!skipFetchState && m_pItem)
			{
				skipFetchState = false;
#warning finally remove this gigamanic function
				fistate = m_pItem->GetStatus(&m_parent.j_sErrorMsg,
						&m_parent.j_nConfirmedSizeSoFar,
						(m_stateInternal == STATE_SEND_MAIN_HEAD) ? &m_parent.j_respHead : nullptr);
			}
			if (fistate >= fileitem::FIST_DLERROR)
				THROW_ERROR(m_parent.j_sErrorMsg);
		}

		try // for bad_alloc in members
		{
			switch (m_stateInternal)
			{
			case (STATE_WAIT_DL_START):
			{
				if (m_eDlType == DLSTATE_NOTNEEDED)
				{
					m_stateInternal = STATE_SEND_MAIN_HEAD;
					continue;
				}

				if (fistate == fileitem::FIST_COMPLETE)
				{
					m_stateInternal = STATE_SEND_MAIN_HEAD;
					continue;
				}
				if (!m_parent.m_pDlClient)
					THROW_ERROR("500 Internal error establishing download");
				// now needs some trigger, either our thread getting IO or another sending updates (then subscribe to updates)

				if (fistate <= fileitem::FIST_DLASSIGNED)
				{
					m_stateExternal = XSTATE_WAIT_DL; // there will be news from downloader
					return;
				}
				// ok, assigned, to whom?
#warning fixme
				//m_eDlType = (pthread_self() == m_parent.dlThreadId) ? DLSTATE_OUR : DLSTATE_OTHER;
				m_stateInternal = STATE_CHECK_DL_PROGRESS;
				m_stateBackSend = STATE_SEND_MAIN_HEAD;
				if (m_eDlType == DLSTATE_OTHER) // ok, not our download agent
				{
					if (!m_parent.Subscribe4updates(m_pItem))
						THROW_ERROR("500 Internal error on progress tracking");
					m_bFitemWasSubscribed = true;
				}
				continue;
			}
			case (STATE_CHECK_DL_PROGRESS):
			{
				if (fistate < fileitem::FIST_DLGOTHEAD)
				{
					m_stateExternal = XSTATE_WAIT_DL;
					return;
				}

				if (m_parent.j_nSendPos >= m_parent.j_nConfirmedSizeSoFar)
				{
					if (fistate >= fileitem::FIST_COMPLETE) // finito...
						GOTOENDE
					;
					LOG("Cannot send more, not enough fresh data yet");
					m_stateExternal = XSTATE_WAIT_DL;
					return;
				}

				m_stateInternal = m_stateBackSend;
				// skipFetchState = true; cannot, will need head data
				continue;
			}
			case (STATE_SEND_MAIN_HEAD):
			{
				ldbg("STATE_FRESH");
				// nothing to send for special codes (like not-unmodified) or w/ GUARANTEED empty body
				int statusCode = m_parent.j_respHead.getStatus();
				bool bResponseHasBody = !BODYFREECODE(statusCode)
						&& (!m_parent.j_respHead.h[header::CONTENT_LENGTH] // not set, maybe chunked data
						|| atoofft(m_parent.j_respHead.h[header::CONTENT_LENGTH])); // set, to non-0

				if (!FormatHeader(fistate, m_pItem->m_nCheckedSize, m_parent.j_respHead, bResponseHasBody,
						statusCode))
				{
					// sErrorMsg was already set
					m_stateInternal = STATE_FATAL_ERROR; // simulated head is prepared but don't send stuff
					continue;
				}

				m_stateInternal = STATE_SEND_BUFFER;
				// just HEAD request was received or no data body to send?
				m_stateBackSend =
						(m_reqHead.type == header::HEAD || !bResponseHasBody) ?
								STATE_FINISHJOB :
								STATE_HEADER_SENT;
				skipFetchState = true;
				USRDBG("Response header to be sent in the next cycle: \n" << m_parent.j_sendbuf);
				continue;
			}
			case (STATE_HEADER_SENT):
			{
				ldbg("STATE_HEADER_SENT");
				m_parent.j_filefd = m_pItem->GetFileFd();
				if (!IsValidFD(m_parent.j_filefd))
					THROW_ERROR("503 IO error");
				m_stateInternal = m_bChunkMode ? STATE_SEND_CHUNK_HEADER : STATE_SEND_PLAIN_DATA;
				ldbg("next state will be: " << (int) m_stateInternal);
				continue;
			}
			case (STATE_SEND_PLAIN_DATA):
			{
				ldbg("STATE_SEND_PLAIN_DATA, max. " << m_parent.j_nConfirmedSizeSoFar);
				auto sendLimit = min(m_parent.j_nConfirmedSizeSoFar,
						m_parent.j_nFileSendLimit + 1);
				auto nMax2SendNow = sendLimit - m_parent.j_nSendPos;
				if(nMax2SendNow == 0)
				{
					m_stateExternal = XSTATE_WAIT_DL;
					return;
				}
#warning could keep writting for long time. Find a sane limiter for max2send now, maybe 3* sockets send buffer
				ldbg("~sendfile: on "<< m_parent.j_nSendPos << " up to : " << nMax2SendNow);
				int n = m_pItem->SendData(confd, m_parent.j_filefd, m_parent.j_nSendPos, nMax2SendNow);
				ldbg("~sendfile: " << n << " new m_nSendPos: " << m_parent.j_nSendPos);

				if (n < 0)
				{
					if(errno == EAGAIN || errno == EINTR)
						return;
					THROW_ERROR("400 Client error");
				}

				m_parent.j_nRetDataCount += n;
				// shortcuts, no need to check state again?
				if (fistate == fileitem::FIST_COMPLETE && m_parent.j_nSendPos == m_parent.j_nConfirmedSizeSoFar)
					GOTOENDE
				;
				if (m_parent.j_nSendPos > m_parent.j_nFileSendLimit)
				{
					m_stateInternal = STATE_FINISHJOB;
					continue;
				}
				m_stateExternal = XSTATE_CAN_SEND;
				return; // probably short IO read or write, give the caller a slot for IO communication now
			}
			case (STATE_SEND_CHUNK_HEADER):
			{
				m_parent.j_nChunkRemainingBytes = min(m_parent.j_nConfirmedSizeSoFar, m_parent.j_nFileSendLimit + 1)
						- m_parent.j_nSendPos;
				ldbg("STATE_SEND_CHUNK_HEADER for " << m_parent.j_nChunkRemainingBytes);
				bool bFinalChunk = !m_parent.j_nChunkRemainingBytes ;

				if (m_parent.j_nChunkRemainingBytes <= 0)
				{
					if(fistate > fileitem::FIST_COMPLETE)
						THROW_ERROR("500 Download interrupted");
					if(fistate < fileitem::FIST_COMPLETE)
					{
						m_stateInternal = STATE_CHECK_DL_PROGRESS;
						m_stateBackSend = STATE_SEND_CHUNK_HEADER;
						continue;
					}
				}
				m_parent.j_sendbuf << tSS::hex << m_parent.j_nChunkRemainingBytes << tSS::dec << sCRLF;
				if(bFinalChunk) m_parent.j_sendbuf << sCRLF;

				m_stateInternal = STATE_SEND_BUFFER;
				m_stateBackSend = bFinalChunk ? STATE_FINISHJOB : STATE_SEND_CHUNK_DATA;
				continue;
			}
			case (STATE_SEND_CHUNK_DATA):
			{
				ldbg("STATE_SEND_CHUNK_DATA");

				if (m_parent.j_nChunkRemainingBytes == 0)
					GOTOENDE
				; // done
				int n = m_pItem->SendData(confd, m_parent.j_filefd, m_parent.j_nSendPos, m_parent.j_nChunkRemainingBytes);
				if (n < 0)
				{
					if(errno == EAGAIN || errno == EINTR)
						return;
					THROW_ERROR("400 Client error");
				}
				m_parent.j_nChunkRemainingBytes -= n;
				m_parent.j_nRetDataCount += n;
				if (m_parent.j_nChunkRemainingBytes <= 0)
				{ // append final newline
					m_parent.j_sendbuf << "\r\n";
					m_stateInternal = STATE_SEND_BUFFER;
					m_stateBackSend = STATE_SEND_CHUNK_HEADER;
				}
				continue; // will check DL availability when reentered
			}
			case (STATE_SEND_BUFFER):
			{
				ldbg("prebuf sending: "<< m_parent.j_sendbuf.c_str());
				auto r = send(confd, m_parent.j_sendbuf.rptr(), m_parent.j_sendbuf.size(), MSG_MORE);
				if (r < 0)
				{
					if(errno == EAGAIN || errno == EINTR || errno == ENOBUFS)
						return;
					m_stateExternal = XSTATE_DISCON;
					return;
				}
				m_parent.j_nRetDataCount += r;
				m_parent.j_sendbuf.drop(r);
				if (m_parent.j_sendbuf.empty())
				{
					USRDBG("Returning to last state, " << (int) m_stateBackSend);
					m_stateInternal = m_stateBackSend;
					continue;
				}
				m_stateExternal = XSTATE_CAN_SEND;
				return; // caller will come back
			}
			case (STATE_FINISHJOB):
			{
				LOG("Reporting job done");
				m_stateExternal = m_bClientWants2Close ? XSTATE_DISCON : XSTATE_FINISHED;
				return;
			}

			case (STATE_FATAL_ERROR):
			{
				// try to send something meaningful, otherwise disconnect

				if (m_parent.j_nRetDataCount > 0) // crap, already started sending or that was the error header
					m_stateExternal = XSTATE_DISCON;
				else
				{
					int hcode = atoi(m_parent.j_sErrorMsg.c_str());
					if (m_parent.j_sErrorMsg.empty() || hcode < 300) // avoid really suspicious things... XXX: report?
						m_parent.j_sErrorMsg.insert(0, "500 Unknown Internal Error ");

					// no fancy error page, this is basically it
					m_parent.j_sendbuf.clear();
					m_parent.j_sendbuf << (m_bIsHttp11 ? "HTTP/1.1 " : "HTTP/1.0 ") << m_parent.j_sErrorMsg	<< sCRLF;
					if(!BODYFREECODE(hcode))
						m_parent.j_sendbuf << "Content-Length: " << ltos(m_parent.j_sErrorMsg.length()) << sCRLF;
					m_parent.j_sendbuf << sCRLF;
					m_stateInternal = STATE_SEND_BUFFER;
					m_stateBackSend = STATE_FINISHJOB; // will abort then
					continue;
				}
			}

			}

		} catch (bad_alloc&)
		{
			// TODO: report memory failure?
			m_stateExternal = XSTATE_DISCON;
		}
	}
}

inline bool job::FormatHeader(const fileitem::FiStatus &fistate, const off_t &nGooddataSize,
		const header& respHead, bool bHasSendableData, int httpstatus)
{
	LOGSTART("job::BuildAndEnqueHeader");

	// just store a copy for the caller
	auto asError = [this](const char *errMsg)
	{
		m_parent.j_sErrorMsg = errMsg;
		return false;
	};

	if (respHead.type != header::ANSWER || respHead.frontLine.length() < 11)
	{
		LOG(respHead.ToString());
		return asError("500 Rotten Data");
	}

	// make sure that header has consistent state and there is data to send which is expected by the client
	LOG("State: " << httpstatus);

	tSS &sb = m_parent.j_sendbuf;
	sb.clear();
	sb << (m_bIsHttp11 ? "HTTP/1.1 " : "HTTP/1.0 ") << respHead.frontLine.c_str() + 9 << sCRLF;

	bool bGotLen(false);

	if (bHasSendableData)
	{
		LOG("has sendable content");
		if (!respHead.h[header::CONTENT_LENGTH]
#ifdef TESTCHUNKMODE
				|| true
#endif
				)
		{
			// unknown length but must have data, will have to improvise: prepare chunked transfer
			if (!m_bIsHttp11) // you cannot process this? go away
				return asError("505 HTTP version not supported for this file");
			m_bChunkMode = true;
			sb << "Transfer-Encoding: chunked\r\n";
		}
		else if (200 == httpstatus) // state: good data response with known length, can try some optimizations
		{
			LOG("has known content length, optimizing response...");

			// Handle If-Modified-Since and Range headers;
			// we deal with them equally but need to know which to use
			const char *pIfmo =
					m_reqHead.h[header::RANGE] ?
							m_reqHead.h[header::IFRANGE] :
							m_reqHead.h[header::IF_MODIFIED_SINCE];
			const char *pLastMo = respHead.h[header::LAST_MODIFIED];

			// consider contents "fresh" for non-volatile data, or when "our" special client is there, or the client simply doesn't care
			bool bDataIsFresh = (m_type != rex::FILE_VOLATILE || m_reqHead.h[header::ACNGFSMARK]
					|| !pIfmo);

			auto tm1 = tm(), tm2 = tm();
			bool bIfModSeenAndChecked = false;
			if (pIfmo && header::ParseDate(pIfmo, &tm1) && header::ParseDate(pLastMo, &tm2))
			{
				time_t a(mktime(&tm1)), b(mktime(&tm2));
				LOG("if-mo-since: " << a << " vs. last-mo: " << b);
				bIfModSeenAndChecked = (a == b);
			}

			// is it fresh? or is this relevant? or is range mode forced?
			if (bDataIsFresh || bIfModSeenAndChecked)
			{
				off_t nContLen = atoofft(respHead.h[header::CONTENT_LENGTH]);

				// Client requested with Range* spec?
				if (m_nReqRangeFrom >= 0)
				{
					if (m_nReqRangeTo < 0 || m_nReqRangeTo >= nContLen) // open-end? set the end to file length. Also when request range would be too large
						m_nReqRangeTo = nContLen - 1;

					// or simply don't care within that rage
					bool bPermitPartialStart = (fistate >= fileitem::FIST_DLGOTHEAD
							&& fistate <= fileitem::FIST_COMPLETE
							&& nGooddataSize >= (m_nReqRangeFrom - cfg::maxredlsize));

					/*
					 * make sure that our client doesn't just hang here while the download thread is
					 * fetching from 0 to start position for many minutes. If the resumed position
					 * is beyond of what we already have, fall back to 200 (complete download).
					 */
					if (fistate == fileitem::FIST_COMPLETE
					// or can start sending within this range (positive range-from)
							|| bPermitPartialStart// don't care since found special hint from acngfs (kludge...)
							|| m_reqHead.h[header::ACNGFSMARK])
					{
						// detect errors, out-of-range case
						if (m_nReqRangeFrom >= nContLen || m_nReqRangeTo < m_nReqRangeFrom)
							return asError("416 Requested Range Not Satisfiable");

						m_parent.j_nSendPos = m_nReqRangeFrom;
						m_parent.j_nFileSendLimit = m_nReqRangeTo;
						// replace with partial-response header
						sb.clear();
#warning test me, get small ranges and fake acngfs calls
						sb << "HTTP/1.1 206 Partial Response\r\nContent-Length: "
								<< (m_parent.j_nFileSendLimit - m_parent.j_nSendPos + 1)
								<< "\r\nContent-Range: bytes " << m_parent.j_nSendPos << "-"
								<< m_parent.j_nFileSendLimit << "/" << nContLen << sCRLF;
						bGotLen = true;
					}
				}
				else if (bIfModSeenAndChecked)
				{
					// file is fresh, and user sent if-mod-since -> fine
					return asError("304 Not Modified");
				}
			}
		}
		// has cont-len available but this header was not set yet in the code above
		if (!bGotLen && !m_bChunkMode)
			sb << "Content-Length: " << respHead.h[header::CONTENT_LENGTH] << sCRLF;

		// OK, has data for user and has set content-length and/or range or chunked transfer mode, now add various meta headers...

		if (respHead.h[header::LAST_MODIFIED])
			sb << "Last-Modified: " << respHead.h[header::LAST_MODIFIED] << sCRLF;

		sb << "Content-Type: ";
		if (respHead.h[header::CONTENT_TYPE])
			sb << respHead.h[header::CONTENT_TYPE] << sCRLF;
		else
			sb << "application/octet-stream\r\n";
	}
	else
	{
		sb << "Content-Length: 0\r\n";
	}

	sb << header::GenInfoHeaders();

	if (respHead.h[header::LOCATION])
		sb << "Location: " << respHead.h[header::LOCATION] << sCRLF;

	if (respHead.h[header::XORIG])
		sb << "X-Original-Source: " << respHead.h[header::XORIG] << sCRLF;

	// whatever the client wants
	sb << "Connection: " << (m_bClientWants2Close ? "close" : "Keep-Alive") << sCRLF;

	//if(!m_bChunkMode)
	sb << sCRLF;

	LOG("response prepared:" << sb);

	return true;
}
void job::SetFatalErrorResponse(cmstring& errorLine)
{
	LOGSTART(__FUNCTION__);
	ldbg(errorLine);
	m_parent.j_sErrorMsg = errorLine;
	if (m_parent.j_nRetDataCount > 0)
	{
		m_stateExternal = XSTATE_DISCON;
		return;
	}
	m_stateInternal = STATE_FATAL_ERROR;
	m_stateExternal = XSTATE_CAN_SEND;
}

}
