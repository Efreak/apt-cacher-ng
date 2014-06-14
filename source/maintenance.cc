

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

#include "debug.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <signal.h>

using namespace MYSTD;

#define MAINT_HTML_DECO "maint.html" 

tSpecialRequest::tSpecialRequest(int fd, tSpecialRequest::eMaintWorkType type) :
	m_reportFD(fd),
	m_szDecoFile(NULL),
	m_mode(type)
{
}

tSpecialRequest::~tSpecialRequest()
{
}

bool tSpecialRequest::SendRawData(const char *data, size_t len, int flags)
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

void tSpecialRequest::SendChunk(const char *data, size_t len, bool bRemoteOnly)
{
	if(!data || !len || m_reportFD<0)
		return;

	if(m_reportFD<3)
	{
		ignore_value(::write(m_reportFD, data, len));
		return;
	}
	// send HTTP chunk header
	char buf[23];
	int l = sprintf(buf, "%x\r\n", (UINT) len);
	SendRawData(buf, l, MSG_MORE | MSG_NOSIGNAL);
	SendRawData(data, len, MSG_MORE | MSG_NOSIGNAL);
	SendRawData("\r\n", 2, MSG_NOSIGNAL);

	if(!bRemoteOnly)
		AfterSendChunk(data, len);
}

void tSpecialRequest::EndTransfer() 
{
	SendRawData(_SZ2PS("0\r\n\r\n"), MSG_NOSIGNAL);
}

void tSpecialRequest::SendChunkedPageHeader(const char *httpcode, const char *mimetype)
{
	tSS s(100);
	s <<  "HTTP/1.1 " << (httpcode ? httpcode : "200") << " OK\r\n"
			"Connection: close\r\n"
			"Transfer-Encoding: chunked\r\n"
			"Content-Type: " << (mimetype?mimetype:"text/html") << "\r\n\r\n";
	SendRawData(s.data(), s.length(), MSG_MORE);
}

class tAuthRequest : public tSpecialRequest
{
public:

	// XXX: c++11 using tSpecialRequest::tSpecialRequest;
	inline tAuthRequest(int fd, tSpecialRequest::eMaintWorkType type)
	: tSpecialRequest(fd, type) {};

	void Run(const string &) override
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

class authbounce : public tSpecialRequest
{
public:

	// XXX: c++11 using tSpecialRequest::tSpecialRequest;
	inline authbounce(int fd, tSpecialRequest::eMaintWorkType type)
	: tSpecialRequest(fd, type) {};


	void Run(const string &) override
	{
		const char authmsg[] = "HTTP/1.1 200 Not Authorized\r\n"
        "Connection: Close\r\n"
        "Content-Type: text/plain\r\nContent-Length: 102\r\n\r\n"
        "Not Authorized. To start this action, an administrator password must be set and "
        "you must be logged in.";
		SendRawData(authmsg, sizeof(authmsg)-1, 0);
	}
};

string & tSpecialRequest::GetHostname()
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

LPCSTR tSpecialRequest::GetTaskName()
{
	switch(m_mode)
	{
	case workNotSpecial: return "ALARM";
	case workExExpire: return "Expiration";
	case workExList: return "Expired Files Listing";
	case workExPurge: return "Expired Files Purging";
	case workExListDamaged: return "Listing Damaged Files";
	case workExPurgeDamaged: return "Truncating Damaged Files";
	case workExTruncDamaged: return "Truncating damaged files to zero size";
	//case workRAWDUMP: /*fall-through*/
	//case workBGTEST: return "42";
	case workUSERINFO: return "General Configuration Information";
	case workTraceStart:
	case workTraceEnd:
	case workMAINTREPORT: return "Status Report and Maintenance Tasks Overview";
	case workAUTHREQUEST: return "Authentication Required";
	case workAUTHREJECT: return "Authentication Denied";
	case workIMPORT: return "Data Import";
	case workMIRROR: return "Archive Mirroring";
	case workDELETE: return "Manual File Deletion";
	case workDELETECONFIRM: return "Manual File Deletion (Confirmed)";
	case workCOUNTSTATS: return "Status Report With Statistics";
	case workSTYLESHEET: return "CSS";
	}
	return "SpecialOperation";
}

tSpecialRequest::eMaintWorkType tSpecialRequest::DispatchMaintWork(cmstring& cmd, const char* auth)
{
	LOGSTARTs("DispatchMaintWork");
	LOG("cmd: " << cmd);

#if 0 // defined(DEBUG)
	if(cmd.find("tickTack")!=stmiss)
	{
		tBgTester(conFD).Run(cmd);
		return;
	}
#endif

	auto spos=cmd.find_first_not_of('/');

	string cssString("style.css");
	if(cmd.substr(spos) == cssString)
		return workSTYLESHEET;

	// starting like the report page?
	if(cmd.compare(spos, acfg::reportpage.length(), acfg::reportpage))
		return workNotSpecial;

	// ok, filename identical, also the end, or having a parameter string?
	auto parmpos=spos+acfg::reportpage.length();
	if(parmpos == cmd.length())
		return workMAINTREPORT;
	// not smaller, was already compared, can be only longer

	if (cmd.at(parmpos) != '?')
		return workNotSpecial; // weird, overlength but not cgi parameter

	// all of the following need authorization if configured, enforce it
	if (!acfg::adminauth.empty())
	{
		if(!auth)
			return workAUTHREQUEST;

		if(acfg::adminauth != auth)
			return workAUTHREJECT;
		// else: confirmed
	}

	struct { LPCSTR trigger; tSpecialRequest::eMaintWorkType type; } matches [] =
	{
			"doExpire=", workExExpire,
			"justShow=", workExList,
			"justRemove=", workExPurge,
			"justShowDamaged=", workExListDamaged,
			"justRemoveDamaged=", workExPurgeDamaged,
			"justTruncDamaged=", workExTruncDamaged,
			"doImport=", workIMPORT,
			"doMirror=", workMIRROR,
			"doDelete=", workDELETE,
			"doDeleteYes=", workDELETE,
			"doCount=", workCOUNTSTATS,
			"doTraceStart=", workTraceStart,
			"doTraceEnd=", workTraceEnd
	};
	for(auto& match: matches)
		if(StrHasFrom(cmd, match.trigger, parmpos))
			return match.type;

	// something weird, go to the main page
	return workMAINTREPORT;
}

tSpecialRequest* tSpecialRequest::MakeMaintWorker(int conFD, eMaintWorkType type)
{
	switch (type)
	{
	case workNotSpecial:
		return NULL;
	case workExExpire:
	case workExList:
	case workExPurge:
	case workExListDamaged:
	case workExPurgeDamaged:
	case workExTruncDamaged:
		return new expiration(conFD, type);
	case workUSERINFO:
		return new tStaticFileSend(conFD, type, "userinfo.html", "text/html", "404 Usage Information");
	case workMAINTREPORT:
	case workCOUNTSTATS:
	case workTraceStart:
	case workTraceEnd:
		return new tStaticFileSend(conFD, type, "report.html", "text/html", "200 OK");
	case workAUTHREQUEST:
		return new tAuthRequest(conFD, type);
	case workAUTHREJECT:
		return new authbounce(conFD, type);
	case workIMPORT:
		return new pkgimport(conFD, type);
	case workMIRROR:
		return new pkgmirror(conFD, type);
	case workDELETE:
	case workDELETECONFIRM:
		return new tDeleter(conFD, type);
	case workSTYLESHEET:
		return new tStaticFileSend(conFD, type, "style.css", "text/css", "200 OK");
	}
	return NULL;
}

void tSpecialRequest::RunMaintWork(eMaintWorkType jobType, cmstring& cmd, int fd)
{
	MYTRY {
		SHARED_PTR<tSpecialRequest> p;
		p.reset(MakeMaintWorker(fd, jobType));
		if(p)
			p->Run(cmd);
	}
	MYCATCH(...)
	{
	}
}
