

#include "meta.h"
#include "lockable.h"

#include "maintenance.h"
#include "expiration.h"
#include "pkgimport.h"
#include "showinfo.h"
#include "jsonstats.h"
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

using namespace std;

#define MAINT_HTML_DECO "maint.html" 

tSpecialRequest::tSpecialRequest(const tRunParms& parms) :
		m_parms(parms)
{
}

tSpecialRequest::~tSpecialRequest()
{
	if(m_bChunkHeaderSent)
		SendRawData(WITHLEN("0\r\n\r\n"), MSG_NOSIGNAL);
}

bool tSpecialRequest::SendRawData(const char *data, size_t len, int flags)
{
	if(m_parms.fd<3) // nothing raw to send for stdout
		return true;

	while(len>0)
	{
		int r=send(m_parms.fd, data, len, flags);
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

void tSpecialRequest::SendChunkRemoteOnly(const char *data, size_t len)
{
	if(!data || !len || m_parms.fd<0)
		return;

	if(m_parms.fd<3)
	{
		ignore_value(::write(m_parms.fd, data, len));
		return;
	}
	// send HTTP chunk header
	char buf[23];
	int l = sprintf(buf, "%x\r\n", (uint) len);
	SendRawData(buf, l, MSG_MORE | MSG_NOSIGNAL);
	SendRawData(data, len, MSG_MORE | MSG_NOSIGNAL);
	SendRawData("\r\n", 2, MSG_NOSIGNAL);
}

void tSpecialRequest::SendChunk(const char *data, size_t len)
{
	SendChunkRemoteOnly(data, len);
	SendChunkLocalOnly(data, len);
}

void tSpecialRequest::SendChunkedPageHeader(const char *httpstatus, const char *mimetype)
{
	tSS s(100);
	s <<  "HTTP/1.1 " << httpstatus << "\r\n"
			"Connection: close\r\n"
			"Transfer-Encoding: chunked\r\n"
			"Content-Type: " << mimetype << "\r\n\r\n";
	SendRawData(s.data(), s.length(), MSG_MORE);
	m_bChunkHeaderSent = true;
}

class tAuthRequest : public tSpecialRequest
{
public:

	// XXX: c++11 using tSpecialRequest::tSpecialRequest;
	inline tAuthRequest(const tSpecialRequest::tRunParms& parms)
	: tSpecialRequest(parms) {};

	void Run() override
	{
		const char authmsg[] = "HTTP/1.1 401 Not Authorized\r\nWWW-Authenticate: "
        "Basic realm=\"For login data, see AdminAuth in Apt-Cacher NG config files\"\r\n"
        "Connection: Close\r\n"
        "Content-Type: text/plain\r\nContent-Length:81\r\n\r\n"
        "Not Authorized. Please contact Apt-Cacher NG administrator for further questions.<br>"
        "<br>"
        "For Admin: Check the AdminAuth option in one of the *.conf files in Apt-Cacher NG "
        "configuration directory, probably " CFGDIR  ;
		SendRawData(authmsg, sizeof(authmsg)-1, 0);
	}
};

class authbounce : public tSpecialRequest
{
public:

	// XXX: c++11 using tSpecialRequest::tSpecialRequest;
	inline authbounce(const tSpecialRequest::tRunParms& parms)
	: tSpecialRequest(parms) {};


	void Run() override
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

		if (0==getsockname(m_parms.fd, (struct sockaddr *)&ss, &slen) && 0
				==getnameinfo((struct sockaddr*) &ss, sizeof(ss), hbuf,
						sizeof(hbuf),
						nullptr, 0, NI_NUMERICHOST))
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
	switch(m_parms.type)
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
	case workTRUNCATE: return "Manual File Truncation";
	case workTRUNCATECONFIRM: return "Manual File Truncation (Confirmed)";
	case workCOUNTSTATS: return "Status Report With Statistics";
	case workSTYLESHEET: return "CSS";
	// case workJStats: return "Stats";
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

	auto epos=cmd.find('?');
	if(epos == stmiss)
		epos=cmd.length();
	auto spos=cmd.find_first_not_of('/');
	auto wlen=epos-spos;

	static string cssString("style.css");
	if(wlen==cssString.length() && 0 == (cmd.compare(spos, wlen, cssString)))
		return workSTYLESHEET;

	// not starting like the maint page?
	if(cmd.compare(spos, wlen, acfg::reportpage))
		return workNotSpecial;

	// ok, filename identical, also the end, or having a parameter string?
	if(epos == cmd.length())
		return workMAINTREPORT;

	// not smaller, was already compared, can be only longer, means having parameters,
	// means needs authorization

	// all of the following need authorization if configured, enforce it
	switch(acfg::CheckAdminAuth(auth))
	{
     case 0:
#ifdef HAVE_CHECKSUM
        break; // auth is ok or no passwort is set
#else
        // most data modifying tasks cannot be run safely without checksumming support 
        return workAUTHREJECT;
#endif
     case 1: return workAUTHREQUEST;
     default: return workAUTHREJECT;
	}

	struct { LPCSTR trigger; tSpecialRequest::eMaintWorkType type; } matches [] =
	{
			{"doExpire=", workExExpire},
			{"justShow=", workExList},
			{"justRemove=", workExPurge},
			{"justShowDamaged=", workExListDamaged},
			{"justRemoveDamaged=", workExPurgeDamaged},
			{"justTruncDamaged=", workExTruncDamaged},
			{"doImport=", workIMPORT},
			{"doMirror=", workMIRROR},
			{"doDelete=", workDELETECONFIRM},
			{"doDeleteYes=", workDELETE},
			{"doTruncate=", workTRUNCATECONFIRM},
			{"doTruncateYes=", workTRUNCATE},
			{"doCount=", workCOUNTSTATS},
			{"doTraceStart=", workTraceStart},
			{"doTraceEnd=", workTraceEnd},
//			{"doJStats", workJStats}
	};
	for(auto& needle: matches)
		if(StrHasFrom(cmd, needle.trigger, epos))
			return needle.type;

	// something weird, go to the maint page
	return workMAINTREPORT;
}

tSpecialRequest* tSpecialRequest::MakeMaintWorker(const tRunParms& parms)
{
	switch (parms.type)
	{
	case workNotSpecial:
		return nullptr;
	case workExExpire:
	case workExList:
	case workExPurge:
	case workExListDamaged:
	case workExPurgeDamaged:
	case workExTruncDamaged:
		return new expiration(parms);
	case workUSERINFO:
		return new tShowInfo(parms);
	case workMAINTREPORT:
	case workCOUNTSTATS:
	case workTraceStart:
	case workTraceEnd:
		return new tMaintPage(parms);
	case workAUTHREQUEST:
		return new tAuthRequest(parms);
	case workAUTHREJECT:
		return new authbounce(parms);
	case workIMPORT:
		return new pkgimport(parms);
	case workMIRROR:
		return new pkgmirror(parms);
	case workDELETE:
	case workDELETECONFIRM:
		return new tDeleter(parms, "Delet");
	case workTRUNCATE:
	case workTRUNCATECONFIRM:
		return new tDeleter(parms, "Truncat");
	case workSTYLESHEET:
		return new tStyleCss(parms);
#if 0
	case workJStats:
		return new jsonstats(parms);
#endif
	}
	return nullptr;
}

void tSpecialRequest::RunMaintWork(eMaintWorkType jobType, cmstring& cmd, int fd)
{
	MYTRY {
		SHARED_PTR<tSpecialRequest> p;
		p.reset(MakeMaintWorker({fd, jobType, cmd}));
		if(p)
			p->Run();
	}
	MYCATCH(...)
	{
	}
}
