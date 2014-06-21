
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
		return SendChunk(ltos(files.size()));
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
	if(PFXCMP(id, len, "cfg:"))
	{
		int bas;
		string key(id+4, len-4);
		auto p=acfg::GetIntPtr(key.c_str(), bas);
		if(p)
			return !*p;
		return -1;
	}
	if(RAWEQ(id, len, "delConfirmed"))
		return m_parms.type != workDELETE;

	return -1;
}

void tMarkupFileSend::SendIfElse(LPCSTR pszBeginSep, LPCSTR pszEnd) {
	auto sep = pszBeginSep;
	auto key=sep+1;
	auto valYes=(LPCSTR) memchr(key, (UINT) *sep, pszEnd-key);
	if(!valYes) // heh?
		return;
	auto sel=CheckCondition(key, valYes-key);
	if(sel<0) // heh?
		return;
	valYes++; // now really there
	auto valNo=(LPCSTR) memchr(valYes, (UINT) *sep, pszEnd-valYes);
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
		string sList;
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
			sList.append(x);
			if(&x != &(*tr.rbegin()))
				sList.append("<br>");
		}
		if(bcount)
			sList.append("<br>some strings not considered due to security restrictions<br>");
		return SendChunk(sList);
	}
	// XXX: could add more generic way, like =cfgvar?value:otherwisevalue
	// but there is not enough need for this yet
	return tMarkupFileSend::SendProp(key);
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

void tMarkupFileSend::SendProp(cmstring &key)
{
	if (startsWithSz(key, "cfg:"))
	{
		string tmp;
		acfg::appendVar(key.c_str() + 4, tmp);
		return SendChunk(tmp);
	}
	if (key == "serverip")
		return SendChunk(GetHostname());
	tSS buf(1024);
	if (key == "footer")
		return SendChunk(GetFooter());
	if (key == "hostname")
	{
		buf.clear();
		auto n = gethostname(buf.wptr(), buf.freecapa());
		if (!n)
			return SendChunk(buf.wptr(), strlen(buf.wptr()));
	}
	if(key=="random")
		return SendChunk(ltos(rand()));
}
