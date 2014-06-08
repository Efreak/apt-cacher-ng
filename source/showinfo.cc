
#include "debug.h"
#include "showinfo.h"
#include "meta.h"
#include "acfg.h"
#include "filereader.h"
#include "fileio.h"

using namespace MYSTD;

const char szReportButton[] =
"<tr><td class=\"colcont\"><form action=\"\" method=\"get\">"
					"<input type=\"submit\" name=\"doCount\" value=\"Count Data\"></form>"
					"</td><td class=\"colcont\" colspan=8 valign=top><font size=-2>"
					"<i>Not calculated, click \"Count data\"</i></font></td></tr>";

// some NOOPs
tStaticFileSend::tStaticFileSend(int fd,
		tSpecialRequest::eMaintWorkType type,
		const char *s,
		const char *m, const char *c)
:
	tSpecialRequest(fd, type),
	m_sFileName(s), m_sMimeType(m), m_sHttpCode(c)
{
}

tStaticFileSend::~tStaticFileSend()
{
}

void tStaticFileSend::ModContents(mstring & contents, cmstring &cmd)
{
	StrSubst(contents, "$SERVERIP", GetHostname());
	StrSubst(contents, "$SERVERPORT", acfg::port.c_str());
	StrSubst(contents, "$REPAGE",  acfg::reportpage);

	tSS footer;
	_AddFooter(footer);
	StrSubst(contents, "$FOOTER", footer);

	if (contents.find("@") != stmiss)
	{
		char buf[1024];
		// ok, needs a set of advanced variables
		gethostname(buf, _countof(buf));
		StrSubst(contents, "@H", buf);
		if (acfg::exfailabort)
			StrSubst(contents, "@A", "checked");
		if (contents.find("@T") != stmiss)
		{
			StrSubst(contents, "@T", cmd.find("doCount") != stmiss
					? aclog::GetStatReport()
					: szReportButton);
		}
	}

}

void tStaticFileSend::Run(const string &cmd)
{
	LOGSTART2("tStaticFileSend::Run", cmd);

	string contents;
	filereader fr;
	if(fr.OpenFile(acfg::confdir+SZPATHSEP+m_sFileName) ||
			(!acfg::suppdir.empty() && fr.OpenFile(acfg::suppdir+SZPATHSEP+m_sFileName)))
	{
		contents.assign(fr.GetBuffer(), fr.GetSize());
		ModContents(contents, cmd);
	}
	else
	{
		contents="Information about APT configuration not available, "
				"please contact the system administrator.";
	}
	tSS buf(1023);
	buf << "HTTP/1.1 " << (m_sHttpCode ? m_sHttpCode : "200 OK")
			<< "\r\nConnection: close\r\nContent-Type: "
			<< (m_sMimeType?m_sMimeType:"text/html")
			<< "\r\nContent-Length: " << contents.length() << "\r\n\r\n";
	SendRawData(buf.rptr(), buf.size(), MSG_MORE);
	SendRawData(contents.data(), contents.length(), 0);
}

void tDeleter::ModContents(mstring & contents, cmstring &cmd)
{
#define BADCHARS "<>\"'|\t"
	tStrPos qpos=cmd.find("?");

	if(cmd.find_first_of(BADCHARS)!=stmiss // what the f..., XSS attempt?
			|| qpos==stmiss)
	{
		contents.clear();
		return;
	}
	tStrVec files;
	tSS sHidParms;
	mstring params(cmd, qpos+1);

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
			contents.clear();
			return;
		}
		if(m_mode == workDELETECONFIRM)
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
	StrSubst(contents, "$COUNT", ltos(files.size()));
	StrSubst(contents, "$STUFF", sHidParms);

	if(m_mode == workDELETE)
	{
		StrSubst(contents, "$VISACTION", "visible");
		StrSubst(contents, "$VISQUESTION", "hidden;height:0px;");
	}
	else // just confirm
	{
		StrSubst(contents, "$VISACTION", "hidden;height:0px;");
		StrSubst(contents, "$VISQUESTION", "visible");
	}
}

