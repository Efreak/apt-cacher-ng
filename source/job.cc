
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

#if 0 // defined(DEBUG)
#define CHUNKDEFAULT true
#warning testing chunk mode
#else
#define CHUNKDEFAULT false
#endif


namespace acng
{

mstring sHttp11("HTTP/1.1");

#define SPECIAL_FD -42
inline bool IsValidFD(int fd) { return fd>=0 || SPECIAL_FD == fd; }

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
class tPassThroughFitem : public fileitem
{
protected:

	// forward buffer, remaining part, cursor position
	const char *m_pData = 0;
	size_t m_nConsumable=0, m_nConsumed=0;
	conn& m_parent;

public:
	tPassThroughFitem(std::string s, conn& par): m_parent(par)
	{
		m_sPathRel = s;
		m_bAllowStoreData=false;
		m_nCheckedSize = m_nSizeSeenInCache = 0;
	};
	virtual FiStatus SetupFromCache(bool) override
	{
		m_nCheckedSize = m_nSizeSeenInCache = 0;
		return m_status = FIST_INITED;
	}
	virtual int GetFileFd() override
			{ return SPECIAL_FD; }; // something, don't care for now
	virtual bool DownloadStartedStoreHeader(const header & h, size_t, const char *,
			bool, bool&) override
	{
		m_head=h;
		m_status=FIST_DLGOTHEAD;
		return true;
	}
	virtual bool StoreFileData(const char *data, unsigned int size) override
	{
		LOGSTART2("tPassThroughFitem::StoreFileData", "status: " << (int) m_status);

#warning fix it too
//		m_nIncommingCount += size;

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
			m_nConsumable=size;
			m_nConsumed=0;

			m_parent.dlPaused = true;

			// let the downloader abort?
			if(m_status >= FIST_DLERROR)
				return false;
		}
		return true;
	}
	ssize_t SendData(int out_fd, int, off_t &nSendPos, size_t nMax2SendNow) override
	{
		lockguard g(m_mx);

		while(0 == m_nConsumable && m_status<=FIST_COMPLETE
				&& ! (m_nCheckedSize==0 && m_status==FIST_COMPLETE))
		{
			return 0; // will get more or block until then
		}
		if (m_status >= FIST_DLERROR || !m_pData)
			return -1;

		if(!m_nCheckedSize)
			return 0;

		auto r = write(out_fd, m_pData, min(nMax2SendNow, m_nConsumable));
		if (r < 0 && (errno == EAGAIN || errno == EINTR)) // harmless
			r = 0;
		if(r<0)
		{
			m_status=FIST_DLERROR;
			m_head.frontLine="HTTP/1.1 500 Data passing error";
			m_parent.dlPaused = false; // needs to shutdown
		}
		else if(r>0)
		{
			m_nConsumable-=r;
			m_pData+=r;
			m_nConsumed+=r;
			nSendPos+=r;
			if(m_nConsumable<=0)
				m_parent.dlPaused = false;
		}
		return r;
	}
};

// base class for a fileitem replacement with custom storage for local data
class tGeneratedFitemBase : public fileitem
{
public:
	virtual int GetFileFd() override
	{ return SPECIAL_FD; }; // something, don't care for now

	tSS m_data;

	tGeneratedFitemBase(const string &sFitemId, cmstring &sFrontLineMsg) : m_data(256)
	{
		m_status=FIST_COMPLETE;
		m_sPathRel=sFitemId;
		m_head.type = header::ANSWER;
		m_head.frontLine = "HTTP/1.1 ";
		m_head.frontLine += sFrontLineMsg.empty()
				? cmstring("500 Internal Failure") : sFrontLineMsg;
		m_head.set(header::CONTENT_TYPE, WITHLEN("text/html") );
	}
	ssize_t SendData(int out_fd, int, off_t &nSendPos, size_t nMax2SendNow)
	override
	{
		if (m_status > FIST_COMPLETE || out_fd<0)
			return -1;
		auto r = m_data.syswrite(out_fd, nMax2SendNow);
		if(r>0) nSendPos+=r;
		return r;
	}
	inline void seal()
	{
		// seal the item
		m_nCheckedSize = m_data.size();
		m_head.set(header::CONTENT_LENGTH, m_nCheckedSize);
	}
	// never used to store data
	bool DownloadStartedStoreHeader(const header &, size_t, const char *, bool, bool&)
	override
	{return false;};
	bool StoreFileData(const char *, unsigned int) override {return false;};
	header & HeadRef() { return m_head; }
};

job::job(header *h, conn &pParent) :
	m_parent(pParent),
	m_pReqHead(h)
{
	LOGSTART2("job::job", "job creating, " << m_pReqHead->frontLine << " and this: " << uintptr_t(this));
}

static const string miscError(" [HTTP error, code: ");

job::~job()
{
	LOGSTART("job::~job");
	int stcode = 555;
	if(m_pItem)
	{
#warning optimize
		m_pItem->GetHeaderLocking().getStatus();
	}

	bool bErr=m_sFileLoc.empty() || stcode >= 400;

#warning fixme, fetching the counts
	m_parent.LogDataCounts(
			m_sFileLoc + (bErr ? (miscError + ltos(stcode) + ']') : sEmptyString),
			m_pReqHead->h[header::XFORWARDEDFOR],
			(m_pItem ? /* m_pItem->GetTransferCount() */ 0 : 0),
			m_nAllDataCount, bErr);
	
#warning when volatile type and finished download, donate this to a quick life-keeping cache for 33s, see cleaner
	// fileitem_with_storage::ProlongLife(m_pItem);
	checkforceclose(m_filefd);
	delete m_pReqHead;

	if(m_pItem && m_bFitemWasSubscribed)
		m_pItem->unsubscribe(m_parent.fdWakeWrite);
}


inline void job::SetupFileServing(const string &visPath,
		const string &fsBase, const string &fsSubpath)
{
	mstring absPath = fsBase+SZPATHSEP+fsSubpath;
	Cstat stbuf(absPath);
	if (!stbuf)
	{
		switch(errno)
		{
		case EACCES:
			SetErrorResponse("403 Permission denied");
			break;
		case EBADF:
		case EFAULT:
		case ENOMEM:
		case EOVERFLOW:
		default:
			//aclog::err("Internal error");
			SetErrorResponse("500 Internal server error");
			break;
		case ELOOP:
			SetErrorResponse("500 Infinite link recursion");
			break;
		case ENAMETOOLONG:
			SetErrorResponse("500 File name too long");
			break;
		case ENOENT:
		case ENOTDIR:
			SetErrorResponse("404 File or directory not found");
			break;
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

	if(S_ISDIR(stbuf.st_mode))
	{
		// unconfuse the browser
		if(!endsWithSzAr(visPath, SZPATHSEPUNIX))
		{
			class dirredirect : public tGeneratedFitemBase
			{
			public:	dirredirect(const string &visPath)
			: tGeneratedFitemBase(visPath, "301 Moved Permanently")
				{
					m_head.set(header::LOCATION, visPath+"/");
					m_data << "<!DOCTYPE html>\n<html lang=\"en\"><head><title>301 Moved Permanently</title></head><body><h1>Moved Permanently</h1>"
					"<p>The document has moved <a href=\""+visPath+"/\">here</a>.</p></body></html>";

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
		auto p=new listing(visPath);
		m_pItem.reset(p);
		tSS & page = p->m_data;

		page << "<!DOCTYPE html>\n<html lang=\"en\"><head><title>Index of "
				<< visPath << "</title></head>"
		"<body><h1>Index of " << visPath << "</h1>"
		"<table><tr><th>&nbsp;</th><th>Name</th><th>Last modified</th><th>Size</th></tr>"
		"<tr><th colspan=\"4\"><hr></th></tr>";

		DIR *dir = opendir(absPath.c_str());
		if (!dir) // weird, whatever... ignore...
			page<<"ERROR READING DIRECTORY";
		else
		{
			// quick hack with sorting by custom keys, good enough here
			priority_queue<tStrPair, std::vector<tStrPair>, std::greater<tStrPair>> sortHeap;
			for(struct dirent *pdp(0);0!=(pdp=readdir(dir));)
			{
				if (0!=::stat(mstring(absPath+SZPATHSEP+pdp->d_name).c_str(), &stbuf))
					continue;

				bool bDir=S_ISDIR(stbuf.st_mode);

				char datestr[32]={0};
				struct tm tmtimebuf;
				strftime(datestr, sizeof(datestr)-1,
						"%d-%b-%Y %H:%M", localtime_r(&stbuf.st_mtime, &tmtimebuf));

				string line;
				if(bDir)
					line += "[DIR]";
				else if(startsWithSz(cfg::GetMimeType(pdp->d_name), "image/"))
					line += "[IMG]";
				else
					line += "[&nbsp;&nbsp;&nbsp;]";
				line += string("</td><td><a href=\"") + pdp->d_name
						+ (bDir? "/\">" : "\">" )
						+ pdp->d_name
						+"</a></td><td>"
						+ datestr
						+ "</td><td align=\"right\">"
						+ (bDir ? string("-") : offttosH(stbuf.st_size));
				sortHeap.push(make_pair(string(bDir?"a":"b")+pdp->d_name, line));
				//dbgprint((mstring)line);
			}
			closedir(dir);
			while(!sortHeap.empty())
			{
				page.add(WITHLEN("<tr><td valign=\"top\">"));
				page << sortHeap.top().second;
				page.add(WITHLEN("</td></tr>\r\n"));
				sortHeap.pop();
			}

		}
		page << "<tr><td colspan=\"4\">" <<GetFooter();
		page << "</td></tr></table></body></html>";
		p->seal();
		return;
	}
	if(!S_ISREG(stbuf.st_mode))
	{
		SetErrorResponse("403 Unsupported data type");
		return;
	}
	/*
	 * This variant of file item handler sends a local file. The
	 * header data is generated as needed, the relative cache path variable
	 * is reused for the real path.
	 */
	class tLocalGetFitem : public fileitem_with_storage
	{
	public:
		tLocalGetFitem(string sLocalPath, struct stat &stdata) :
			fileitem_with_storage(sLocalPath)
		{
			m_bAllowStoreData=false;
			m_status=FIST_COMPLETE;
			m_nCheckedSize=m_nSizeSeenInCache=stdata.st_size;
			m_bCheckFreshness=false;
			m_head.type=header::ANSWER;
			m_head.frontLine="HTTP/1.1 200 OK";
			m_head.set(header::CONTENT_LENGTH, stdata.st_size);
			m_head.prep(header::LAST_MODIFIED, 26);
			if(m_head.h[header::LAST_MODIFIED])
				FormatTime(m_head.h[header::LAST_MODIFIED], 26, stdata.st_mtim.tv_sec);
			cmstring &sMimeType=cfg::GetMimeType(sLocalPath);
			if(!sMimeType.empty())
				m_head.set(header::CONTENT_TYPE, sMimeType);
		};
		virtual int GetFileFd() override
		{
			int fd=open(m_sPathRel.c_str(), O_RDONLY);
		#ifdef HAVE_FADVISE
			// optional, experimental
			if(fd>=0)
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

	const char *pRange = m_pReqHead->h[header::RANGE];
	// working around a bug in old curl versions
	if (!pRange)
		pRange = m_pReqHead->h[header::CONTENT_RANGE];
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

void job::PrepareDownload(LPCSTR headBuf)
{

	LOGSTART("job::PrepareDownload");

#ifdef DEBUGLOCAL
	cfg::localdirs["stuff"]="/tmp/stuff";
	log::err(m_pReqHead->ToString());
#endif


	auto _SwitchToPtItem = [this]() ->fileitem::FiStatus
	{
		// Changing to local pass-through file item
		LOGSTART("job::_SwitchToPtItem");
		m_pItem.reset(new tPassThroughFitem(m_sFileLoc, m_parent));
		m_pItem->m_nSizeSeenInCache = m_nReqRangeFrom;
	#warning XXX: test resuming
		m_pItem->m_nRangeLimit = m_nReqRangeTo;
		m_pItem->m_head.set(header::LAST_MODIFIED,
				m_pReqHead->h[header::IF_MODIFIED_SINCE] ?
						m_pReqHead->h[header::IF_MODIFIED_SINCE] : m_pReqHead->h[header::IFRANGE]);
		return (m_pItem->m_status = fileitem::FIST_INITED);
	};

	string sReqPath, sPathResidual;
	tHttpUrl theUrl; // parsed URL

	// resolve to an internal repo location and maybe backends later
	cfg::tRepoResolvResult repoMapping;

	fileitem::FiStatus fistate(fileitem::FIST_FRESH);
	bool bForceFreshnessChecks(false); // force update of the file, i.e. on dynamic index files?
	tSplitWalk tokenizer(&m_pReqHead->frontLine, SPACECHARS);

	if (m_pReqHead->type != header::GET && m_pReqHead->type != header::HEAD)
		goto report_invpath;
	if (!tokenizer.Next() || !tokenizer.Next()) // at path...
		goto report_invpath;
	UrlUnescapeAppend(tokenizer, sReqPath);
	if (!tokenizer.Next()) // at proto
		goto report_invpath;
	m_bIsHttp11 = (sHttp11 == tokenizer.str());

	USRDBG("Decoded request URI: " << sReqPath);

	// a sane default? normally, close-mode for http 1.0, keep for 1.1.
	// But if set by the client then just comply!
	m_bClientWants2Close = !m_bIsHttp11;
	if (m_pReqHead && m_pReqHead->h[header::CONNECTION])
		m_bClientWants2Close = !strncasecmp(m_pReqHead->h[header::CONNECTION], "close", 5);

	// "clever" file system browsing attempt?
	if (rex::Match(sReqPath, rex::NASTY_PATH) || stmiss != sReqPath.find(MAKE_PTR_0_LEN("/_actmp"))
	|| startsWithSz(sReqPath, "/_"))
		goto report_notallowed;

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
				goto report_invport;
		}

		if (cfg::pUserPorts)
		{
			if (!cfg::pUserPorts->test(nPort))
				goto report_invport;
		}
		else if (nPort != 80)
			goto report_invport;

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
						m_pReqHead->h[header::AUTHORIZATION]);
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
					goto report_notallowed;

				// ok, collect some information helpful to the user
				m_type = FILE_VOLATILE;
/*				lockguard g(traceData);
				traceData.insert(theUrl.sPath);

				*/
			}
		}

		// got something valid, has type now, trace it
		USRDBG("Processing new job, "<<m_pReqHead->frontLine);

		cfg::GetRepNameAndPathResidual(theUrl, repoMapping);
		if (repoMapping.psRepoName && !repoMapping.psRepoName->empty())
			m_sFileLoc = *repoMapping.psRepoName + SZPATHSEP + repoMapping.sRestPath;
		else
			m_sFileLoc = theUrl.sHost + theUrl.sPath;

		bForceFreshnessChecks = (!cfg::offlinemode && m_type == FILE_VOLATILE);

		m_pItem = fileitem_with_storage::CreateRegistered(m_sFileLoc);
	}
	catch (std::out_of_range&) // better safe...
	{
		goto report_invpath;
	}

	if (!m_pItem)
	{
		USRDBG("Error creating file item for " << m_sFileLoc);
		goto report_overload;
	}

	if (cfg::DegradedMode())
		goto report_degraded;

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
			|| (m_pReqHead->type == header::HEAD && 0 != (m_nReqRangeTo = -1)))
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
		goto report_offlineconf;
	}
	dbgline;
	if (fistate < fileitem::FIST_DLASSIGNED) // needs a downloader
	{
		dbgline;
		if (!m_parent.SetupDownloader(m_pReqHead->h[header::XFORWARDEDFOR]))
		{
			USRDBG("Error creating download handler for "<<m_sFileLoc);
			goto report_overload;
		}

		dbgline;
		try
		{
			auto bHaveRedirects =
					(repoMapping.repodata && !repoMapping.repodata->m_backends.empty());

			if (cfg::forcemanaged && !bHaveRedirects)
				goto report_notallowed;

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
				m_parent.m_lastDlState.flags |= dlcon::tWorkState::needConnect;
			}
			else
			{
				ldbg("PANIC! Error creating download job for " << m_sFileLoc);
				goto report_overload;
			}
		} catch (std::bad_alloc&) // OOM, may this ever happen here?
		{
			USRDBG("Out of memory");
			goto report_overload;
		};
	}

	return;

	report_notallowed: SetErrorResponse(
			(tSS() << "403 Forbidden file type or location: " << sReqPath).c_str(), nullptr,
			"403 Forbidden file type or location");
//    USRDBG( sRawUriPath + " -- ACCESS FORBIDDEN");
	return;

	report_offlineconf: SetErrorResponse("503 Unable to download in offline mode");
	return;

	report_invpath: SetErrorResponse("403 Invalid path specification");
	return;

	report_degraded: SetErrorResponse("403 Cache server in degraded mode");
	return;

	report_invport: SetErrorResponse(
			"403 Configuration error (confusing proxy mode) or prohibited port (see AllowUserPorts)");
	return;
	/*
	 report_doubleproxy:
	 SetErrorResponse("403 URL seems to be made for proxy but contains apt-cacher-ng port. "
	 "Inconsistent apt configuration?");
	 return ;
	 */
	setup_pt_request:
	{
		m_sFileLoc = theUrl.ToURI(false);
		ParseRange();
		_SwitchToPtItem();
		if (!m_parent.SetupDownloader(m_pReqHead->h[header::XFORWARDEDFOR]))
		{
			USRDBG("Error creating download handler for "<<m_sFileLoc);
			goto report_overload;
		}
		if (m_parent.m_pDlClient->AddJob(m_pItem, &theUrl, repoMapping.repodata, nullptr,
				headBuf, cfg::redirmax))
		{
			ldbg("Download job enqueued for " << m_sFileLoc);
			m_parent.m_lastDlState.flags |= dlcon::tWorkState::needConnect;
		}
		else
		{
			ldbg("PANIC! Error creating download job for " << m_sFileLoc);
			goto report_overload;
		}
	}
	return;

	report_overload: SetErrorResponse("503 Server overload, try later");
	return;
}

#define REPORT_ERROR { m_stateInternal = STATE_FATAL_ERROR; continue;}
#define THROW_ERROR(x) { sErrorMsg = x; REPORT_ERROR; }

void job::SendData(int confd)
{
	LOGSTART("job::SendData");

	// finish this way if no better state found
	m_stateExternal = XSTATE_DISCON;

	if(m_eMaintWorkType)
	{
		tSpecialRequest::RunMaintWork(m_eMaintWorkType, m_sFileLoc, confd);
		return;
	}
	
	if(!m_pItem)
		return;

#if 0
	fileitem::FiStatus fistate(fileitem::FIST_DLERROR);
	header respHead; // template of the response header, e.g. as found in cache

	if (confd<0)
		return XSTATE_DISCON; // shouldn't get here with any legal path

	for(;;)
	{
		fistate=m_pItem->GetStatusUnlocked(nGoodDataSize);
		if (fistate > fileitem::FIST_COMPLETE)
		{
			const header &h = m_pItem->GetHeaderUnlocked();
			g.unLock(); // item lock must be released in order to replace it!
			if(m_nAllDataCount)
				return XSTATE_DISCON;
			if(h.h[header::XORIG])
				m_sOrigUrl=h.h[header::XORIG];
			// must be something like "HTTP/1.1 403 Forbidden"
			if(h.frontLine.length()>9 && h.getStatus()!=200)
				SetErrorResponse(h.frontLine.c_str()+9, h.h[header::LOCATION]);
			else // good ungood? confused somewhere?!
			{
				ldbg("good ungood, consused?" << h.frontLine)
				SetErrorResponse("500 Unknown error");
			}
			return R_AGAIN; // will send error response and die
		}
		/*
		 * Detect the same special case as above. There is no download agent to change
		 * the state so no need to wait.
		*/
		if(m_bNoDownloadStarted)
		{
			fistate = fileitem::FIST_DLRECEIVING;
			break;
		}
		// or wait for the dl source to get data at the position we need to start from
		LOG("sendstate: " << (int) fistate << " , sendpos: " << m_nSendPos << " from " << nGoodDataSize);
		if(fistate==fileitem::FIST_COMPLETE || (m_nSendPos < nGoodDataSize && fistate>=fileitem::FIST_DLGOTHEAD))
			break;

		ldbg("cannot send more data for " << m_sFileLoc);

#if 0
			// if this is our downloader, don't block it when it needs our thread cpu time
			if(m_pParentCon->m_pDlClient
					&& m_pParentCon->m_pDlClient->IsCurrentDownload(m_pItem.getFiPtr())
			&& m_pParentCon->m_lastDlState.flags)// & (dlcon::tWorkState::needConnect | dlcon::tWorkState::needRecv | dlcon::tWorkState::needSend)))
				return R_AGAIN;

#endif
			// if downloader is active, don't block it
#warning bad idea, will block although the dler could continue with another in the queue and vice versa... PROBLEM! IDEA NEEDED.
			if(m_pParentCon->m_lastDlState.flags & dlcon::tWorkState::needActivity)
				return R_WAITDL;
			// otherwise it's some other downloader active, just wait for progress
			m_pItem->wait(g);
			
			dbgline;
		}

	respHead = m_pItem->GetHeaderUnlocked();
#warning what for??
	if(respHead.h[header::XORIG])
		m_sOrigUrl=respHead.h[header::XORIG];
#endif

	// fetch the data we need mostly
#warning move this stuff to a scratch area inside of m_parent (!!)
	string sErrorMsg;
	fileitem::FiStatus fistate = fileitem::FIST_FRESH;
	pthread_t dlThreadId;
	header respHead;

	// can avoid data checks when switching around, but only once
	bool skipFetchState = false;

	// eof?
#define GOTOENDE { m_stateInternal = STATE_FINISHJOB ; skipFetchState = true; continue; }

	while(true) // send initial header, chunk header, etc. in a sequence; eventually left by returning
	{
		if(!skipFetchState)
		{
			skipFetchState = false;

			fistate = m_pItem->GetStatus(&dlThreadId, &sErrorMsg, &m_nConfirmedSizeSoFar,
					(m_stateInternal == STATE_SEND_MAIN_HEAD)
					? &respHead : nullptr);
		}
		if(fistate >= fileitem::FIST_DLERROR)
			THROW_ERROR(sErrorMsg);

		try // for bad_alloc in members
		{
			switch(m_stateInternal)
			{
			case(STATE_WAIT_DL_START):
			{
				if(m_eDlType == DLSTATE_NOTNEEDED)
				{
					m_stateInternal = STATE_SEND_MAIN_HEAD;
					continue;
				}

				if(fistate == fileitem::FIST_COMPLETE)
				{
					m_stateInternal = STATE_SEND_MAIN_HEAD;
					continue;
				}
				if(!m_parent.m_pDlClient)
					THROW_ERROR("500 Internal error establishing download");
				// now needs some trigger, either our thread getting IO or another sending updates (then subscribe to updates)

				if(fistate <= fileitem::FIST_DLASSIGNED)
				{
					m_stateExternal = XSTATE_WAIT_DL; // there will be news from downloader
					return;
				}
				// ok, assigned, to whom?
				m_eDlType = (pthread_self() == dlThreadId) ? DLSTATE_OUR : DLSTATE_OTHER;
				m_stateInternal = STATE_CHECK_DL_PROGRESS;
				m_stateBackSend = STATE_SEND_MAIN_HEAD;
				if(m_eDlType == DLSTATE_OTHER) // ok, not our download agent
				{
					m_parent.SetupSubscription(m_pItem);
					m_bFitemWasSubscribed = true;
				}
				continue;
}
			case(STATE_CHECK_DL_PROGRESS):
	{
				if(fistate < fileitem::FIST_DLGOTHEAD)
				{
					m_stateExternal = XSTATE_WAIT_DL;
					return;
				}

				if(m_nSendPos>=m_nConfirmedSizeSoFar)
				{
					if(fistate>=fileitem::FIST_COMPLETE) // finito...
						GOTOENDE;
					LOG("Cannot send more, not enough fresh data yet");
					m_stateExternal = XSTATE_WAIT_DL;
					return;
				}

				m_stateInternal = m_stateBackSend;
				skipFetchState = true;
				continue;
	}
				case(STATE_SEND_MAIN_HEAD):
				{
#warning fixme, keine extra pruefung von fistate... wie komme ich her, alles checken
					ldbg("STATE_FRESH");
#warning wird bool und soll ueber serror melden
					if(!BuildAndEnqueHeader(fistate, m_pItem->m_nCheckedSize, respHead))
						THROW_ERROR(sErrorMsg);
#warning brauche alternativ-macro wenn msg schon dort steht
					m_stateInternal = STATE_SEND_BUFFER;
					m_stateBackSend = STATE_HEADER_SENT; // could be changed while creating header
					skipFetchState = true;
					USRDBG("Response header to be sent in the next cycle: \n" << m_sendbuf );
					continue;
				}
				case(STATE_HEADER_SENT):
				{
					ldbg("STATE_HEADER_SENT");
					m_filefd=m_pItem->GetFileFd();
					if(!IsValidFD(m_filefd)) THROW_ERROR("503 IO error");
					m_stateInternal = m_bChunkMode ? STATE_SEND_CHUNK_HEADER : STATE_SEND_PLAIN_DATA;
					ldbg("next state will be: " << (int) m_stateInternal);
					continue;
				}
				case(STATE_SEND_PLAIN_DATA):
				{
					ldbg("STATE_SEND_PLAIN_DATA, max. " << m_nConfirmedSizeSoFar);

					size_t nMax2SendNow=min(m_nConfirmedSizeSoFar - m_nSendPos, m_nFileSendLimit + 1 - m_nSendPos);
					ldbg("~sendfile: on "<< m_nSendPos << " up to : " << nMax2SendNow);
					int n = m_pItem->SendData(confd, m_filefd, m_nSendPos, nMax2SendNow);
					ldbg("~sendfile: " << n << " new m_nSendPos: " << m_nSendPos);

					if(n<0)
						THROW_ERROR("400 Client error");

					m_nAllDataCount += n;
					// shortcuts, no need to check state again?
					if(fistate==fileitem::FIST_COMPLETE && m_nSendPos==m_nConfirmedSizeSoFar)
						GOTOENDE;
					if(m_nSendPos>m_nFileSendLimit)
					{
						m_stateInternal = STATE_FINISHJOB;
						continue;
					}
					m_stateExternal = XSTATE_CAN_SEND;
					return; // probably short IO read or write, give the caller a slot for IO communication now
				}
				case(STATE_SEND_CHUNK_HEADER):
				{
					m_nChunkRemainingBytes=min(m_nConfirmedSizeSoFar - m_nSendPos, m_nFileSendLimit + 1 - m_nSendPos);
					ldbg("STATE_SEND_CHUNK_HEADER for " << m_nChunkRemainingBytes);
					if(m_nChunkRemainingBytes <= 0)
					{
						m_stateInternal = STATE_CHECK_DL_PROGRESS;
						m_stateBackSend = STATE_SEND_CHUNK_HEADER;
						continue;
					}
					m_sendbuf << tSS::hex << m_nChunkRemainingBytes << tSS::dec
							<< (m_nChunkRemainingBytes ? "\r\n" : "\r\n\r\n");

					m_stateInternal=STATE_SEND_BUFFER;
					m_stateBackSend=m_nChunkRemainingBytes ? STATE_SEND_CHUNK_DATA : STATE_FINISHJOB;
					continue;
				}
				case(STATE_SEND_CHUNK_DATA):
				{
					ldbg("STATE_SEND_CHUNK_DATA");

					if(m_nChunkRemainingBytes==0)
						GOTOENDE; // done
					int n = m_pItem->SendData(confd, m_filefd, m_nSendPos, m_nChunkRemainingBytes);
					if(n<0)
						THROW_ERROR("400 Client error");
					m_nChunkRemainingBytes-=n;
					m_nAllDataCount+=n;

#warning test it, fix it... end handling = spaghetti
					if(m_nChunkRemainingBytes<=0)
					{ // append final newline
						m_sendbuf<<"\r\n";
						m_stateInternal=STATE_SEND_BUFFER;
						m_stateBackSend=STATE_SEND_CHUNK_HEADER;
					}
					continue; // will check DL availability when reentered
				}
				case(STATE_SEND_BUFFER):
				{
					ldbg("prebuf sending: "<< m_sendbuf.c_str());
					auto r=send(confd, m_sendbuf.rptr(), m_sendbuf.size(), MSG_MORE);
					if (r<0)
					{
						if (errno==EAGAIN || errno==EINTR || errno == ENOBUFS)
						{
							m_stateExternal = XSTATE_CAN_SEND;
							return; // try again later
						}
						THROW_ERROR("500 Communication problem");
					}
					m_nAllDataCount+=r;
					m_sendbuf.drop(r);
					if(m_sendbuf.empty())
					{
						USRDBG("Returning to last state, " << (int) m_stateBackSend);
						m_stateInternal=m_stateBackSend;
						continue;
					}
					m_stateExternal = XSTATE_CAN_SEND;
					return; // caller will come back
				}
/*
				case(STATE_ALLDONE):
					LOG("State: STATE_ALLDONE?");
				// no break
				case (STATE_ERRORCONT):
					LOG("or STATE_ERRORCONT?");
					*/
				// no break
				case(STATE_FINISHJOB):
			{
				LOG("Reporting job done");
				m_stateExternal = m_bClientWants2Close ? XSTATE_DISCON : XSTATE_FINISHED;
				return;
			}

				case(STATE_FATAL_ERROR):
	{
					if(m_nAllDataCount > 0) // crap, already started sending
						m_stateExternal = XSTATE_DISCON;
					else
					{
						SetErrorResponse(sErrorMsg);
					}
	}

			}

		}
		catch(bad_alloc&) {
			// TODO: report memory failure?
			m_stateExternal = XSTATE_DISCON;
		}
	}
}


inline const char * job::BuildAndEnqueHeader(const fileitem::FiStatus &fistate,
		const off_t &nGooddataSize, header& respHead)
{
	LOGSTART("job::BuildAndEnqueHeader");

	if(respHead.type != header::ANSWER)
	{
		LOG(respHead.ToString());
		return "500 Rotten Data";
	}

	// make sure that header has consistent state and there is data to send which is expected by the client
	int httpstatus = respHead.getStatus();
	LOG("State: " << httpstatus);

	// nothing to send for special codes (like not-unmodified) or w/ GUARANTEED empty body
	bool bHasSendableData = !BODYFREECODE(httpstatus)
			&& (!respHead.h[header::CONTENT_LENGTH] // not set, maybe chunked data
			                || atoofft(respHead.h[header::CONTENT_LENGTH])); // set, to non-0

	tSS &sb=m_sendbuf;
	sb.clear();
	sb << respHead.frontLine <<"\r\n";

	bool bGotLen(false);

	if(bHasSendableData)
	{
		LOG("has sendable content");
		if( ! respHead.h[header::CONTENT_LENGTH]
#ifdef DEBUG // might have been defined before for testing purposes
		                 || m_bChunkMode
#endif
				)
		{
			// unknown length but must have data, will have to improvise: prepare chunked transfer
			if ( ! m_bIsHttp11 ) // you cannot process this? go away
				return "505 HTTP version not supported for this file";
			m_bChunkMode=true;
			sb<<"Transfer-Encoding: chunked\r\n";
		}
		else if(200==httpstatus) // state: good data response with known length, can try some optimizations
		{
			LOG("has known content length, optimizing response...");

			// Handle If-Modified-Since and Range headers;
			// we deal with them equally but need to know which to use
			const char *pIfmo = m_pReqHead->h[header::RANGE] ?
					m_pReqHead->h[header::IFRANGE] : m_pReqHead->h[header::IF_MODIFIED_SINCE];
			const char *pLastMo = respHead.h[header::LAST_MODIFIED];

			// consider contents "fresh" for non-volatile data, or when "our" special client is there, or the client simply doesn't care
			bool bDataIsFresh = (m_type != rex::FILE_VOLATILE
				|| m_pReqHead->h[header::ACNGFSMARK] || !pIfmo);

			auto tm1=tm(), tm2=tm();
			bool bIfModSeenAndChecked=false;
			if(pIfmo && header::ParseDate(pIfmo, &tm1) && header::ParseDate(pLastMo, &tm2))
			{
				time_t a(mktime(&tm1)), b(mktime(&tm2));
				LOG("if-mo-since: " << a << " vs. last-mo: " << b);
				bIfModSeenAndChecked = (a==b);
			}

			// is it fresh? or is this relevant? or is range mode forced?
			if(  bDataIsFresh || bIfModSeenAndChecked)
			{
				off_t nContLen=atoofft(respHead.h[header::CONTENT_LENGTH]);

				// Client requested with Range* spec?
				if(m_nReqRangeFrom >=0)
				{
					if(m_nReqRangeTo<0 || m_nReqRangeTo>=nContLen) // open-end? set the end to file length. Also when request range would be too large
						m_nReqRangeTo=nContLen-1;

					// or simply don't care within that rage
					bool bPermitPartialStart = (
							fistate >= fileitem::FIST_DLGOTHEAD
							&& fistate <= fileitem::FIST_COMPLETE
							&& nGooddataSize >= ( m_nReqRangeFrom - cfg::maxredlsize));

					/*
					 * make sure that our client doesn't just hang here while the download thread is
					 * fetching from 0 to start position for many minutes. If the resumed position
					 * is beyond of what we already have, fall back to 200 (complete download).
					 */
					if(fistate==fileitem::FIST_COMPLETE
							// or can start sending within this range (positive range-from)
							|| bPermitPartialStart	// don't care since found special hint from acngfs (kludge...)
							|| m_pReqHead->h[header::ACNGFSMARK] )
					{
						// detect errors, out-of-range case
						if(m_nReqRangeFrom>=nContLen || m_nReqRangeTo<m_nReqRangeFrom)
							return "416 Requested Range Not Satisfiable";

						m_nSendPos = m_nReqRangeFrom;
						m_nFileSendLimit = m_nReqRangeTo;
						// replace with partial-response header
						sb.clear();
#warning test me, get small ranges and fake acngfs calls
						sb << "HTTP/1.1 206 Partial Response\r\nContent-Length: "
						 << (m_nFileSendLimit-m_nSendPos+1) <<
								"\r\nContent-Range: bytes "<< m_nSendPos
								<< "-" << m_nFileSendLimit << "/" << nContLen << "\r\n";
						bGotLen=true;
					}
				}
				else if(bIfModSeenAndChecked)
				{
					// file is fresh, and user sent if-mod-since -> fine
					return "304 Not Modified";
				}
			}
		}
		// has cont-len available but this header was not set yet in the code above
		if( !bGotLen && !m_bChunkMode)
			sb<<"Content-Length: "<<respHead.h[header::CONTENT_LENGTH]<<"\r\n";

		// OK, has data for user and has set content-length and/or range or chunked transfer mode, now add various meta headers...

		if(respHead.h[header::LAST_MODIFIED])
			sb<<"Last-Modified: "<<respHead.h[header::LAST_MODIFIED]<<"\r\n";

		sb<<"Content-Type: ";
		if(respHead.h[header::CONTENT_TYPE])
			sb<<respHead.h[header::CONTENT_TYPE]<<"\r\n";
		else
			sb<<"application/octet-stream\r\n";
	}
	else
	{
		sb<<"Content-Length: 0\r\n";

		m_stateBackSend = STATE_FINISHJOB;
	}

	sb << header::GenInfoHeaders();

	if(respHead.h[header::LOCATION])
		sb<<"Location: "<<respHead.h[header::LOCATION]<<"\r\n";

	if(!m_sOrigUrl.empty())
		sb<<"X-Original-Source: "<< m_sOrigUrl <<"\r\n";

	// whatever the client wants
	sb<<"Connection: "<<(m_bClientWants2Close?"close":"Keep-Alive")<<"\r\n";

	sb<<"\r\n";
	LOG("response prepared:" << sb);

	if(m_pReqHead->type==header::HEAD)
		m_stateBackSend = STATE_FINISHJOB; // simulated head is prepared but don't send stuff

	return 0;
}


void job::SetErrorResponse(cmstring& errorLine, const char *szLocation, const char *bodytext)
{
	LOGSTART2("job::SetErrorResponse", errorLine << " ; for " << m_sOrigUrl);
	class erroritem: public tGeneratedFitemBase
	{
	public:
		erroritem(const string &sId, cmstring& sError, const char *bodytext)
			: tGeneratedFitemBase(sId, sError)
		{
			if(BODYFREECODE(m_head.getStatus()))
				return;
			// otherwise do something meaningful
			m_data << "<!DOCTYPE html>\n<html lang=\"en\"><head><title>"
					<< sError << "</title>\n</head>\n<body><h1>";
			if (bodytext)
				m_data << bodytext;
			else
				m_data << sError;
			m_data << "</h1></body></html>";
			m_head.set(header::CONTENT_TYPE, "text/html");
			seal();
		}
	};

	erroritem *p = new erroritem("noid", errorLine, bodytext);
	p->HeadRef().set(header::LOCATION, szLocation);
	if(m_bFitemWasSubscribed)
	{
		m_pItem->unsubscribe(m_parent.fdWakeWrite);
		m_bFitemWasSubscribed = false;
	}
	m_pItem.reset(p);
	//aclog::err(tSS() << "fileitem is now " << uintptr_t(m_pItem.get()));
	m_stateInternal=STATE_SEND_MAIN_HEAD;
}

}
