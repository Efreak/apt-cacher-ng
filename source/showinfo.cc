
#include "debug.h"
#include "showinfo.h"
#include "meta.h"
#include "acfg.h"
#include "filereader.h"
#include "fileio.h"
#include "job.h"

using namespace std;

static cmstring sReportButton("<tr><td class=\"colcont\"><form action=\"#stats\" method=\"get\">"
					"<input type=\"submit\" name=\"doCount\" value=\"Count Data\"></form>"
					"</td><td class=\"colcont\" colspan=8 valign=top><font size=-2>"
					"<i>Not calculated, click \"Count data\"</i></font></td></tr>");
static cmstring sDisabled("disabled");
static cmstring block("block"), none("none"), sInline("inline");

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
		m_bFatalError = ! ( fr.OpenFile(acfg::confdir+SZPATHSEP+m_sFileName, true) ||
			(!acfg::suppdir.empty() && fr.OpenFile(acfg::suppdir+SZPATHSEP+m_sFileName, true)));
	}
	if(m_bFatalError)
	{
		m_sHttpCode="500 Template Not Found";
		m_sMimeType="text/plain";
		return SendRaw(errstring.c_str(), errstring.size());
	}

	pr = fr.GetBuffer();
	pend = pr + fr.GetSize();

	SendChunkedPageHeader(m_sHttpCode, m_sMimeType);

	auto lastchar=pend-1;
	while(pr<pend)
	{
		auto restlen=pend-pr;
		auto propStart=(LPCSTR) memchr(pr, (UINT) '$', restlen);
		if (propStart) {
			if (propStart < lastchar && propStart[1] == '{') {
				SendChunk(pr, propStart-pr);
				pr=propStart;
				// found begin of a new property key
				auto propEnd = (LPCSTR) memchr(propStart+2, (UINT) '}', pend-propStart+2);
				if(!propEnd)// unclosed, seriously? Just dump the rest at the user
					goto no_more_props;
				string key(propStart+2, propEnd-propStart-2);
				SendChunk(GetProp(key, sEmptyString));
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

cmstring& tDeleter::GetProp(cmstring &key, cmstring& altValue)
{
	if(key=="count")
		return KeepCopy(ltos(files.size()));
	if(key == "stuff")
		return KeepCopy(sHidParms);
	if(key == "blockIfConfirmed")
		return m_parms.type == workDELETE ? block : none;
	if(key == "noneIfConfirmed")
		return m_parms.type == workDELETE ? none : block;

	return tMarkupFileSend::GetProp(key, altValue);
}

tDeleter::tDeleter(const tRunParms& parms)
: tMarkupFileSend(parms, "delconfirm.html", "text/html", "200 OK")
{
#define BADCHARS "<>\"'|\t"
	tStrPos qpos=m_parms.cmd.find("?");

	if(m_parms.cmd.find_first_of(BADCHARS) not_eq stmiss // what the f..., XSS attempt?
			|| qpos==stmiss)
	{
		m_bFatalError=true;
		return;
	}

	mstring params(m_parms.cmd, qpos+1);

	for(tSplitWalk split(&params, "&"); split.Next();)
	{
		char *sep(0);
		mstring tok(split);
		if(startsWithSz(tok, "kf")
				&& strtoul(tok.c_str()+2, &sep, 10)>0
				&& sep && '=' == *sep)
		{
			files.push_back(UrlUnescape(sep+1));
		}
	}

	// do stricter path checks and prepare the query page data

	UINT lfd(1);
	for(const auto path : files)
	{
		if(path.find_first_of(BADCHARS)!=stmiss  // what the f..., XSS attempt?
		 || rechecks::Match(path, rechecks::NASTY_PATH))
		{
			m_bFatalError=true;
			return;
		}
		if(m_parms.type  == workDELETECONFIRM)
		{
			sHidParms << "<input type=\"hidden\" name=\"kf" << ++lfd << "\" value=\""
					<< path <<"\">\n";
		}
		else
		{
			sHidParms<<"Deleting " << path<<"<br>\n";
			::unlink((acfg::cacheDirSlash+path).c_str());
			::unlink((acfg::cacheDirSlash+path+".head").c_str());
		}
	}
}

tMaintPage::tMaintPage(const tRunParms& parms)
:tMarkupFileSend(parms, "report.html", "text/html", "200 OK")
{

	if(StrHas(parms.cmd, "doTraceStart"))
		acfg::patrace=true;
	else if(StrHas(parms.cmd, "doTraceStop"))
		acfg::patrace=false;
	else if(StrHas(parms.cmd, "doTraceClear"))
	{
		auto& tr(tTraceData::getInstance());
		lockguard g(tr);
		tr.clear();
	}

}

cmstring& tMaintPage::GetProp(cmstring &key, cmstring& altValue)
{
	if(key=="statsRow")
	{
		if(!StrHas(m_parms.cmd, "doCount"))
			return sReportButton;
		scratch.push_back(aclog::GetStatReport());
		return scratch.back();
	}
	static cmstring defStringChecked("checked");
	if(key == "aOeDefaultChecked")
		return acfg::exfailabort ? defStringChecked : sEmptyString;
	if(key == "curPatTraceCol")
	{
		scratch.push_back(sEmptyString);
		auto& tr(tTraceData::getInstance());
		lockguard g(tr);
		int bcount=0;
		for(auto& x: tr)
		{
			if(x.find_first_of(BADCHARS) not_eq stmiss)
			{
				bcount++;
				continue;
			}
			scratch.back().append(x);
			if(&x != &(*tr.rbegin()))
				scratch.back().append("<br>");
		}
		if(bcount)
			scratch.back().append("some strings not considered due to security restrictions<br>");
		return scratch.back();
	}
	// XXX: could add more generic way, like =cfgvar?value:otherwisevalue
	// but there is not enough need for this yet
	if(key=="inlineIfPatrace")
		return acfg::patrace ? sInline : none;
	if(key=="noneIfPatrace")
		return !acfg::patrace ? sInline : none;

	if(key == "ifNotTracingDisabled")
		return acfg::patrace ? sEmptyString : sDisabled;
	if(key == "ifTracingDisabled")
			return acfg::patrace ? sDisabled : sEmptyString;

	return tMarkupFileSend::GetProp(key, altValue);
}

void tMarkupFileSend::SendRaw(const char* pBuf, size_t len)
{
	tSS buf(10230);
	// go the easy way if nothing to replace there
	buf << "HTTP/1.1 " << (m_sHttpCode ? m_sHttpCode : "200 OK")
			<< "\r\nConnection: close\r\nContent-Type: "
			<< (m_sMimeType ? m_sMimeType : "text/html")
			<< "\r\nContent-Length: " << len
			<< "\r\n\r\n";
	SendRawData(buf.rptr(), buf.size(), MSG_MORE);
	SendRawData(pBuf, len, 0);
}

cmstring& tMarkupFileSend::GetProp(cmstring &key, cmstring& altValue)
{
	if (startsWithSz(key, "cfg:"))
	{
		scratch.push_back(sEmptyString);
		acfg::appendVar(key.c_str() + 4, scratch.back());
		return scratch.back();
	}
	if (key == "serverip")
		return GetHostname();
	tSS buf(1024);
	if (key == "footer")
		return GetFooter();
	if (key == "hostname")
	{
		buf.clear();
		auto n = gethostname(buf.wptr(), buf.freecapa());
		if (!n)
			return KeepCopy(buf.wptr());
	}
	if(key=="random")
		return KeepCopy(ltos(rand()));

	return altValue;
}
