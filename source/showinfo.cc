
#include "debug.h"
#include "showinfo.h"
#include "meta.h"
#include "acfg.h"
#include "filereader.h"
#include "fileio.h"
#include "job.h"

#include <iostream>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

using namespace std;

#ifdef SendFmt
// just be sure about that, its buffer is used for local purposes and
// so it shall not interfere with tFmtSendObj
#undef SendFmt
#undef SendFmtRemote
#endif

#define SCALEFAC 250


namespace acng
{

static cmstring sReportButton("<tr><td class=\"colcont\"><form action=\"#stats\" method=\"get\">"
					"<input type=\"submit\" name=\"doCount\" value=\"Count Data\"></form>"
					"</td><td class=\"colcont\" colspan=8 valign=top><font size=-2>"
					"<i>Not calculated, click \"Count data\"</i></font></td></tr>");

// some NOOPs
tMarkupFileSend::tMarkupFileSend(const tSpecialRequest::tRunParms& parms,
		const char *s,
		const char *m, const char *c)
:
	tSpecialRequest(parms),
	m_sFileName(s), m_sMimeType(m), m_sHttpCode(c)
{
}

static cmstring errstring("Information about APT configuration not available, "
		"please contact the system administrator.");

void tMarkupFileSend::Run()
{
	LOGSTART2("tStaticFileSend::Run", m_parms.cmd);

	filereader fr;
	const char *pr(nullptr), *pend(nullptr);
	if(!m_bFatalError)
	{
		m_bFatalError = ! ( fr.OpenFile(cfg::confdir+SZPATHSEP+m_sFileName, true) ||
			(!cfg::suppdir.empty() && fr.OpenFile(cfg::suppdir+SZPATHSEP+m_sFileName, true)));
	}
	if(m_bFatalError)
	{
		m_sHttpCode="500 Template Not Found";
		m_sMimeType="text/plain";
		return SendRaw(errstring.data(), (size_t) errstring.size());
	}

	pr = fr.GetBuffer();
	pend = pr + fr.GetSize();

	SendChunkedPageHeader(m_sHttpCode, m_sMimeType);

	auto lastchar=pend-1;
	while(pr<pend)
	{
		auto restlen=pend-pr;
		auto propStart=(LPCSTR) memchr(pr, (uint) '$', restlen);
		if (propStart) {
			if (propStart < lastchar && propStart[1] == '{') {
				SendChunk(pr, propStart-pr);
				pr=propStart;
				// found begin of a new property key
				auto propEnd = (LPCSTR) memchr(propStart+2, (uint) '}', pend-propStart+2);
				if(!propEnd)// unclosed, seriously? Just dump the rest at the user
					goto no_more_props;
				if(propStart+6<propEnd && ':' == *(propStart+2))
					SendIfElse(propStart+3, propEnd);
				else
				{
					string key(propStart+2, propEnd-propStart-2);
					SendProp(key);
				}
				pr=propEnd+1;
			} else {
				// not a property string, send as-is
				propStart++;
				SendChunk(pr, propStart-pr);
				pr=propStart;
			}
		} else // no more props
		{
			no_more_props:
			SendChunk(pr, restlen);
			break;
		}
	}
}

void tDeleter::SendProp(cmstring &key)
{
	if(key=="count")
		return SendChunk(m_fmtHelper.clean()<<files.size());
	else if(key=="countNZs")
	{
		if(files.size()!=1)
			return SendChunk(m_fmtHelper.clean()<<"s");
	}
	else if(key == "stuff")
		return SendChunk(sHidParms);
	else if(key=="vmode")
		return SendChunk(sVisualMode.data(), sVisualMode.size());
	return tMarkupFileSend::SendProp(key);
}

// and deserialize it from GET parameter into m_delCboxFilter

tDeleter::tDeleter(const tRunParms& parms, const mstring& vmode)
: tMarkupFileSend(parms, "delconfirm.html", "text/html", "200 OK"),
  sVisualMode(vmode)
{
#define BADCHARS "<>\"'|\t"
	tStrPos qpos=m_parms.cmd.find("?");

	if(m_parms.cmd.find_first_of(BADCHARS) not_eq stmiss // what the f..., XSS attempt?
			|| qpos==stmiss)
	{
		m_bFatalError=true;
		return;
	}

	auto del = (m_parms.type == workDELETE);
	mstring params(m_parms.cmd, qpos+1);
	mstring blob;
	for(tSplitWalk split(params, "&", true); split.Next();)
	{
		mstring tok(split);
#warning can use view here. OTOH this whole method should be replaced with DB-based operation, user gets to see only the request id
		if(startsWithSz(tok, "kf="))
		{
			char *end(0);
			auto val = strtoul(tok.c_str()+3, &end, 36);
			if(*end == 0 || *end=='&')
#ifdef COMPATGCC47                           
				files.insert(val);
#else
				files.emplace(val);
#endif
		}
		else if(startsWithSz(tok, "blob="))
			tok.swap(blob);
	}
	sHidParms << "<input type=\"hidden\" name=\"blob\" value=\"";
	if(!blob.empty())
		sHidParms.append(blob.data()+5, blob.size()-5);
	sHidParms <<  "\">\n";

	tStrDeq filePaths;
	acbuf buf;
	mstring redoLink;

#ifdef HAVE_DECB64 // this page isn't accessible with crippled configuration anyway
	if (!blob.empty())
	{
		// let's decode the blob and pickup what counts
		tSS gzBuf;
		//if(!Hex2buf(blob.data()+5, blob.size()-5, gzBuf)) return;
		if (!DecodeBase64(blob.data()+5, blob.size()-5, gzBuf)) return;
		if(gzBuf.size() < 2 * sizeof(unsigned)) return;
		unsigned ulen = 123456;
		memcpy(&ulen, gzBuf.rptr(), sizeof(unsigned));
		if (ulen > 100000) // no way...
			return;
		gzBuf.drop(sizeof(unsigned));
		buf.setsize(ulen);
		uLongf uncompSize = ulen;
		auto gzCode = uncompress((Bytef*) buf.wptr(), &uncompSize, (const Bytef*) gzBuf.rptr(),
				gzBuf.size());
		if (Z_OK != gzCode)
			return;
		buf.got(uncompSize);
	}
#endif
	while(true)
	{
		unsigned id, slen;
		if(buf.size() < 2*sizeof(unsigned))
			break;
		memcpy(&id, buf.rptr(), sizeof(unsigned));
		buf.drop(sizeof(unsigned));
		memcpy(&slen, buf.rptr(), sizeof(unsigned));
		buf.drop(sizeof(unsigned));
		if(slen > buf.size()) // looks fishy
			return;
		if(redoLink.empty()) // don't care about id in the first line
			redoLink.assign(buf.rptr(), slen);
		else if(ContHas(files, id))
			filePaths.emplace_back(buf.rptr(), slen);
		buf.drop(slen);
	}

	// do stricter path checks and prepare the query page data
	for(const auto& path : filePaths)
	{
		if(path.find_first_of(BADCHARS)!=stmiss  // what the f..., XSS attempt?
		 || rex::Match(path, rex::NASTY_PATH))
		{
			m_bFatalError=true;
			return;
		}
	}

	// XXX: this is wasting some CPU cycles but is good enough for this case
	for (const auto& path : filePaths)
	{
		mstring bname(path);
		for(const auto& sfx: sfxXzBz2GzLzma)
			if(endsWith(path, sfx))
				bname = path.substr(0, path.size()-sfx.size());
		auto tryAdd=[this,&bname,&path](cmstring& sfx)
				{
					auto cand = bname+sfx;
					if(cand == path || ::access(SZABSPATH(cand), F_OK))
						return;
					extraFiles.push_back(cand);
				};
		for(const auto& sfx: sfxMiscRelated)
			tryAdd(sfx);
		if(endsWith(path, relKey))
			tryAdd(path.substr(0, path.size()-relKey.size())+inRelKey);
		if(endsWith(path, inRelKey))
			tryAdd(path.substr(0, path.size()-inRelKey.size())+relKey);
	}


	if (m_parms.type == workDELETECONFIRM || m_parms.type == workTRUNCATECONFIRM)
	{
		for (const auto& path : filePaths)
			sHidParms << html_sanitize(path) << "<br>\n";
		for (const auto& pathId : files)
			sHidParms << "<input type=\"hidden\" name=\"kf\" value=\"" <<
			to_base36(pathId) << "\">\n";
		if(m_parms.type == workDELETECONFIRM && !extraFiles.empty())
		{
			sHidParms << sBRLF << "<b>Extra files found</b>" << sBRLF
					<< "<p>It's recommended to delete the related files (see below) as well, otherwise "
					<< "the removed files might be resurrected by recovery mechanisms later.<p>"
					<< "<input type=\"checkbox\" name=\"cleanRelated\" value=\"1\" checked=\"checked\">"
					<< "Yes, please remove all related files<p>Example list:<p>";
			for (const auto& path : extraFiles)
				sHidParms << path << sBRLF;
		}
	}
	else
	{
		for (const auto& path : filePaths)
		{
			auto doFile=[this, &del](cmstring& path)
					{
				for (auto suf : { "", ".head" })
				{
					sHidParms << (del ? "Deleting " : "Truncating ") << path << suf << "<br>\n";
					auto p = cfg::cacheDirSlash + path + suf;
					int r = del ? unlink(p.c_str()) : truncate(p.c_str(), 0);
					if (r && errno != ENOENT)
					{
						tErrnoFmter ferrno("<span class=\"ERROR\">[ error: ");
						sHidParms << ferrno << " ]</span>" << sBRLF;
					}
					if(!del)
						break;
				}
			};
			doFile(path);
			if(StrHas(m_parms.cmd, "cleanRelated="))
				for (const auto& path : extraFiles)
					doFile(path);

		}
		sHidParms << "<br><a href=\""<< redoLink << "\">Repeat the last action</a><br>" << sBRLF;
	}
}

tMaintPage::tMaintPage(const tRunParms& parms)
:tMarkupFileSend(parms, "report.html", "text/html", "200 OK")
{

	if(StrHas(parms.cmd, "doTraceStart"))
		cfg::patrace=true;
	else if(StrHas(parms.cmd, "doTraceStop"))
		cfg::patrace=false;
	else if(StrHas(parms.cmd, "doTraceClear"))
	{
		auto& tr(tTraceData::getInstance());
		lockguard g(tr);
		tr.clear();
	}
}

// compares c string with a determined part of another string
#define RAWEQ(longstring, len, pfx) (len==(sizeof(pfx)-1) && 0==memcmp(longstring, pfx, sizeof(pfx)-1))
#define PFXCMP(longstring, len, pfx) ((sizeof(pfx)-1) <= len && 0==memcmp(longstring, pfx, sizeof(pfx)-1))

inline int tMarkupFileSend::CheckCondition(LPCSTR id, size_t len)
{
	//std::cerr << "check if: " << string(id, len) << std::endl;
	if(PFXCMP(id, len, "cfg:"))
	{
		string key(id+4, len-4);
		auto p=cfg::GetIntPtr(key.c_str());
		if(p)
			return ! *p;
		if(key == "degraded")
			return cfg::DegradedMode();
    	return -1;
	}
	if(RAWEQ(id, len, "delConfirmed"))
		return m_parms.type != workDELETE && m_parms.type != workTRUNCATE;

	return -2;
}

void tMarkupFileSend::SendIfElse(LPCSTR pszBeginSep, LPCSTR pszEnd)
{
	//std::cerr << "got if: " << string(pszBeginSep, pszEnd-pszBeginSep) << std::endl;
	auto sep = pszBeginSep;
	auto key=sep+1;
	auto valYes=(LPCSTR) memchr(key, (uint) *sep, pszEnd-key);
	if(!valYes) // heh?
		return;
	auto sel=CheckCondition(key, valYes-key);
	//std::cerr << "sel: " << sel << std::endl;
	if(sel<0) // heh?
		return;
	valYes++; // now really there
	auto valNo=(LPCSTR) memchr(valYes, (uint) *sep, pszEnd-valYes);
	//std::cerr << "valNO: " << valNo<< std::endl;
	if(!valNo) // heh?
			return;
	if(0==sel)
		SendChunk(valYes, valNo-valYes);
	else
		SendChunk(valNo+1, pszEnd-valNo-1);
}


void tMaintPage::SendProp(cmstring &key)
{
	if(key=="statsRow")
	{
		if(!StrHas(m_parms.cmd, "doCount"))
			return SendChunk(sReportButton);
		return SendChunk(log::GetStatReport());
	}
	static cmstring defStringChecked("checked");
	if(key == "aOeDefaultChecked")
		return SendChunk(cfg::exfailabort ? defStringChecked : sEmptyString);
	if(key == "curPatTraceCol")
	{
		m_fmtHelper.clear();
		auto& tr(tTraceData::getInstance());
		lockguard g(tr);
		int bcount=0;
		for(cmstring& x: tr)
		{
			if(x.find_first_of(BADCHARS) not_eq stmiss)
			{
				bcount++;
				continue;
			}
			m_fmtHelper<<x;
			if(&x != &(*tr.rbegin()))
				m_fmtHelper.append(WITHLEN("<br>"));
		}
		if(bcount)
			m_fmtHelper.append(WITHLEN("<br>some strings not considered due to security restrictions<br>"));
		return SendChunk(m_fmtHelper);
	}
	return tMarkupFileSend::SendProp(key);
}

void tMarkupFileSend::SendRaw(const char* pBuf, size_t len)
{
	// go the easy way if nothing to replace there
	m_fmtHelper.clean() << "HTTP/1.1 " << (m_sHttpCode ? m_sHttpCode : "200 OK")
			<< "\r\nConnection: close\r\nContent-Type: "
			<< (m_sMimeType ? m_sMimeType : "text/html")
			<< "\r\nContent-Length: " << len
			<< "\r\n\r\n";
	SendRawData(m_fmtHelper.rptr(), m_fmtHelper.size(), MSG_MORE | MSG_NOSIGNAL);
	SendRawData(pBuf, len, MSG_NOSIGNAL);
}

void tMarkupFileSend::SendProp(cmstring &key)
{
	if (startsWithSz(key, "cfg:"))
	{
		auto ckey=key.c_str() + 4;
		auto ps(cfg::GetStringPtr(ckey));
		if(ps)
			return SendChunk(*ps);
		auto pi(cfg::GetIntPtr(ckey));
		if(pi)
			return SendChunk(m_fmtHelper.clean() << *pi);
		return;
	}
	if (key == "serverhostport")
		return SendChunk(GetMyHostPort());
	if (key == "footer")
		return SendChunk(GetFooter());

	if (key == "hostname")
	{
		m_fmtHelper.clean().setsize(500);
		if(gethostname(m_fmtHelper.wptr(), m_fmtHelper.freecapa()))
			return; // failed?
		return SendChunk(m_fmtHelper.wptr(), strlen(m_fmtHelper.wptr()));
	}
	if(key=="random")
		return SendChunk(m_fmtHelper.clean() << rand());
	if(key=="dataInHuman")
	{
		auto stats = log::GetCurrentCountersInOut();
		return SendChunk(offttosH(stats.first));
	}
	if(key=="dataOutHuman")
	{
		auto stats = log::GetCurrentCountersInOut();
		return SendChunk(offttosH(stats.second));
	}
	if(key=="dataIn")
	{
		auto stats = log::GetCurrentCountersInOut();
		auto statsMax = std::max(stats.first, stats.second);
		auto pixels = statsMax ? (stats.first * SCALEFAC / statsMax) : 0;
		return SendChunk(m_fmtHelper.clean() << pixels);
	}
	if(key=="dataOut")
	{
		auto stats = log::GetCurrentCountersInOut();
		auto statsMax = std::max(stats.second, stats.first);
		auto pixels = statsMax ? (SCALEFAC * stats.second / statsMax) : 0;
		return SendChunk(m_fmtHelper.clean() << pixels);
	}

	if (key == "dataHistInHuman")
	{
		auto stats = pairSum(log::GetCurrentCountersInOut(), log::GetOldCountersInOut());
		return SendChunk(offttosH(stats.first));
	}
	if (key == "dataHistOutHuman")
	{
		auto stats = pairSum(log::GetCurrentCountersInOut(), log::GetOldCountersInOut());
		return SendChunk(offttosH(stats.second));
	}
	if (key == "dataHistIn")
	{
		auto stats = pairSum(log::GetCurrentCountersInOut(), log::GetOldCountersInOut());
		auto statsMax = std::max(stats.second, stats.first);
		auto pixels = statsMax ? (stats.first * SCALEFAC / statsMax) : 0;
		return SendChunk(m_fmtHelper.clean() << pixels);
	}
	if (key == "dataHistOut")
	{
		auto stats = pairSum(log::GetCurrentCountersInOut(), log::GetOldCountersInOut());
		auto statsMax = std::max(stats.second, stats.first);
		auto pixels = statsMax ? (SCALEFAC * stats.second/statsMax) : 0;
		return SendChunk(m_fmtHelper.clean() << pixels);
	}


}

}
