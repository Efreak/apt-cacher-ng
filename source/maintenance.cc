

#include "meta.h"
#include "lockable.h"

#include "maintenance.h"
#include "expiration.h"
#include "pkgimport.h"
#include "showinfo.h"
#include "mirror.h"
#include "aclogger.h"
#include "filereader.h"
#include "acfg.h"
#include "acbuf.h"
#include "sockio.h"
#include "caddrinfo.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <signal.h>

using namespace MYSTD;

#define MAINT_HTML_DECO "maint.html" 

tWUIPage::tWUIPage(int fd) :
	m_reportFD(fd),
	m_szDecoFile(NULL)
{
}

tWUIPage::~tWUIPage()
{
}

bool tWUIPage::SendRawData(const char *data, size_t len, int flags)
{
	if(m_reportFD<3) // nothing raw to send for stdout
		return true;

	while(len>0)
	{
		int r=send(m_reportFD, data, len, flags);
		if(r<0)
		{
			if(errno==EINTR || errno==EAGAIN)
				r=0;
			else
				return false;
		}
		
		data+=r;
		len-=r;
	}
	return true;
}

void tWUIPage::SendChunk(const char *data, size_t len, bool bRemoteOnly)
{
	if(!data || !len || m_reportFD<0)
		return;

	if(m_reportFD<3)
	{
		ignore_value(::write(m_reportFD, data, len));
		return;
	}

	char buf[23];
	int l=sprintf(buf, "%x\r\n", (UINT) len);
	SendRawData(buf, l, MSG_MORE|MSG_NOSIGNAL);
	SendRawData(data, len, MSG_MORE|MSG_NOSIGNAL);
	SendRawData("\r\n", 2, MSG_NOSIGNAL);

	if(!bRemoteOnly)
		AfterSendChunk(data, len);
}

void tWUIPage::EndTransfer() 
{
	SendRawData(_SZ2PS("0\r\n\r\n"), MSG_NOSIGNAL);
}

void tWUIPage::SendChunkedPageHeader(const char *httpcode, const char *mimetype)
{
	tSS s(100);
	s <<  "HTTP/1.1 " << (httpcode ? httpcode : "200") << " OK\r\n"
			"Connection: close\r\n"
			"Transfer-Encoding: chunked\r\n"
			"Content-Type: " << (mimetype?mimetype:"text/html") << "\r\n\r\n";
	SendRawData(s.data(), s.length(), MSG_MORE);
}

class tAuthRequest : public tWUIPage
{
public:
	tAuthRequest(int fd) : tWUIPage(fd)
	{
	}
	void Run(const string &)
	{
		const char authmsg[] = "HTTP/1.1 401 Not Authorized\r\nWWW-Authenticate: "
        "Basic realm=\"For login data, see AdminAuth in Apt-Cacher NG config files\"\r\n"
        "Connection: Close\r\n"
        "Content-Type: text/plain\r\nContent-Length:81\r\n\r\n"
        "Not Authorized. Please contact Apt-Cacher NG administrator for further questions.<br>"
        "<br>"
        "For Admin: Check the AdminAuth option in one of the *.conf files in Apt-Cacher NG "
        "configuration directory, probably /etc/apt-cacher-ng/." ;
		SendRawData(authmsg, sizeof(authmsg)-1, 0);
	}
};

class authbounce : public tWUIPage
{
public:
	authbounce(int fd) : tWUIPage(fd)
	{
	}
	void Run(const string &)
	{
		const char authmsg[] = "HTTP/1.1 200 Not Authorized\r\n"
        "Connection: Close\r\n"
        "Content-Type: text/plain\r\nContent-Length: 102\r\n\r\n"
        "Not Authorized. To start this action, an administrator password must be set and "
        "you must be logged in.";
		SendRawData(authmsg, sizeof(authmsg)-1, 0);
	}
};

string & tWUIPage::GetHostname()
{
	if (m_sHostname.empty())
	{
		struct sockaddr_storage ss;
		socklen_t slen = sizeof(ss);
		char hbuf[NI_MAXHOST];

		if (0==getsockname(m_reportFD, (struct sockaddr *)&ss, &slen) && 0
				==getnameinfo((struct sockaddr*) &ss, sizeof(ss), hbuf,
						sizeof(hbuf),
						NULL, 0, NI_NUMERICHOST))
		{
			const char *p=hbuf;
			bool bAddBrs(false);
			if(0==strncmp(hbuf, "::ffff:", 7) && strpbrk(p, "0123456789."))
				p+=7; // no more colons there, looks like v4 IP in v6 space -> crop it
			else if(strchr(p, (int) ':'))
				bAddBrs=true; // full v6 address for sure, add brackets

			if(bAddBrs)
				m_sHostname="[";
			m_sHostname+=p;
			if(bAddBrs)
				m_sHostname+="]";
		}
		else
			m_sHostname="IP-of-this-cache-server";
	}
	return m_sHostname;
}

void DispatchAndRunMaintTask(cmstring &cmd, int conFD, const char * szAuthLine)
{
	tWUIPage *pWorker(NULL);
	if(!szAuthLine)
		szAuthLine="";

#ifdef DEBUG
		if(cmd.find("tickTack")!=stmiss)
		{
			tBgTester(conFD).Run(cmd);
			return;
		}
#endif

	// not effective, why? signal(SIGPIPE, SIG_IGN);
	
	MYTRY
	{
		//MYSTD::cout << "vgl: " << authLine << " und " << acfg::adminauth << "\n";
		// admin actions are passed with GET parameters, appended after ?
		tStrPos qpos=cmd.find('?');
		if(qpos!=stmiss)
		{
			if( ! acfg::adminauth.empty() && acfg::adminauth!=szAuthLine )
				pWorker = new tAuthRequest(conFD);
			else if(StrHas(cmd, "doExpire="))
				pWorker = new expiration(conFD, expiration::expire);
			else if(StrHas(cmd, "justShow="))
				pWorker = new expiration(conFD, expiration::list);
			else if(StrHas(cmd, "justRemove="))
				pWorker = new expiration(conFD, expiration::purge);
			else if(StrHas(cmd, "justShowDamaged="))
				pWorker = new expiration(conFD, expiration::listDamaged);
			else if(StrHas(cmd, "justRemoveDamaged="))
				pWorker = new expiration(conFD, expiration::purgeDamaged);
			else if(StrHas(cmd, "justTruncDamaged="))
				pWorker = new expiration(conFD, expiration::truncDamaged);
			else if(cmd.find("doImport=")!=stmiss)
				pWorker=new pkgimport(conFD);
			else if(cmd.find("doMirror=")!=stmiss)
			{
				if(acfg::adminauth.empty())
					pWorker=new authbounce(conFD);
				else
					pWorker=new pkgmirror(conFD);
			}
			else if(cmd.find("doDelete=")!=stmiss ||cmd.find("doDeleteYes=")!=stmiss)
			{
				if(acfg::adminauth.empty())
					pWorker=new authbounce(conFD);
				else
					pWorker=new tDeleter(conFD);
			}
			else if(cmd.find("doCount=")!=stmiss)
				pWorker = new tStaticFileSend(conFD, "report.html", "text/html", "200 OK");
			else // ok... just show the default page
				goto l_show_cnc;
		}
		else if (cmd.compare(1, acfg::reportpage.length(), acfg::reportpage) == 0)
		{
			l_show_cnc:
			pWorker = new tStaticFileSend(conFD, "report.html", "text/html", "200 OK");
		}
		else if (cmd == "/style.css")
			pWorker = new tStaticFileSend(conFD, "style.css", "text/css", "200 OK");
		else
			pWorker = new tStaticFileSend(conFD, "userinfo.html", "text/html",
					"404 Usage Information");

		if(pWorker)
			pWorker->Run(cmd);
	}
	MYCATCH(...)
	{ /* whatever */ };
	delete pWorker;
}
