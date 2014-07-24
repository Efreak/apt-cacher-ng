
#include "debug.h"
#include "showinfo.h"
#include "meta.h"
#include "acfg.h"
#include "filereader.h"
#include "fileio.h"
#include "job.h"

#include <iostream>

using namespace std;

#ifdef SendFmt
// just be sure about that, its buffer is used for local purposes and
// so it shall not interfere with tFmtSendObj
#undef SendFmt
#undef SendFmtRemote
#endif

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
		m_bFatalError = ! ( fr.OpenFile(acfg::confdir+SZPATHSEP+m_sFileName, true) ||
			(!acfg::suppdir.empty() && fr.OpenFile(acfg::suppdir+SZPATHSEP+m_sFileName, true)));
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
		auto propStart=(LPCSTR) memchr(pr, (UINT) '$', restlen);
		if (propStart) {
			if (propStart < lastchar && propStart[1] == '{') {
				SendChunk(pr, propStart-pr);
				pr=propStart;
				// found begin of a new property key
				auto propEnd = (LPCSTR) memchr(propStart+2, (UINT) '}', pend-propStart+2);
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
	if(key == "stuff")
		return SendChunk(sHidParms);
	return tMarkupFileSend::SendProp(key);
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

// compares c string with a determined part of another string
#define RAWEQ(longstring, len, pfx) (len==(sizeof(pfx)-1) && 0==memcmp(longstring, pfx, sizeof(pfx)-1))
#define PFXCMP(longstring, len, pfx) ((sizeof(pfx)-1) <= len && 0==memcmp(longstring, pfx, sizeof(pfx)-1))

inline int tMarkupFileSend::CheckCondition(LPCSTR id, size_t len)
{
	//std::cerr << "check if: " << string(id, len) << std::endl;
	if(PFXCMP(id, len, "cfg:"))
	{
		string key(id+4, len-4);
		auto p=acfg::GetIntPtr(key.c_str());
		if(p)
			return ! *p;
    if(key == "degraded")
       return acfg::degraded.load();
		return -1;
	}
	if(RAWEQ(id, len, "delConfirmed"))
		return m_parms.type != workDELETE;

	return -2;
}

void tMarkupFileSend::SendIfElse(LPCSTR pszBeginSep, LPCSTR pszEnd)
{
	//std::cerr << "got if: " << string(pszBeginSep, pszEnd-pszBeginSep) << std::endl;
	auto sep = pszBeginSep;
	auto key=sep+1;
	auto valYes=(LPCSTR) memchr(key, (UINT) *sep, pszEnd-key);
	if(!valYes) // heh?
		return;
	auto sel=CheckCondition(key, valYes-key);
	//std::cerr << "sel: " << sel << std::endl;
	if(sel<0) // heh?
		return;
	valYes++; // now really there
	auto valNo=(LPCSTR) memchr(valYes, (UINT) *sep, pszEnd-valYes);
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
		return SendChunk(aclog::GetStatReport());
	}
	static cmstring defStringChecked("checked");
	if(key == "aOeDefaultChecked")
		return SendChunk(acfg::exfailabort ? defStringChecked : sEmptyString);
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
		auto ps(acfg::GetStringPtr(ckey));
		if(ps)
			return SendChunk(*ps);
		auto pi(acfg::GetIntPtr(ckey));
		if(pi)
			return SendChunk(m_fmtHelper.clean() << *pi);
		return;
	}
	if (key == "serverip")
		return SendChunk(GetHostname());
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
}
