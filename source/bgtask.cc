/*
 * bgtask.cpp
 *
 *  Created on: 18.09.2009
 *      Author: ed
 */

#include <cstring>

#include "bgtask.h"

#include "acfg.h"
#include "meta.h"
#include "filereader.h"

#include <limits.h>
#include <errno.h>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

using namespace std;

#define LOG_DECO_START "<html><head><style type=\"text/css\">" \
".WARNING { color: orange; }\n.ERROR { color: red; }\n" \
"</style></head><body>"
#define LOG_DECO_END "</body></html>"

namespace acng
{

// for start/stop and abort hint
base_with_condition tSpecOpDetachable::g_StateCv;
bool tSpecOpDetachable::g_sigTaskAbort=false;
// not zero if a task is active
time_t nBgTimestamp = 0;

tSpecOpDetachable::~tSpecOpDetachable()
{
	if(m_reportStream.is_open())
	{
		m_reportStream << LOG_DECO_END;
		m_reportStream.close();
	}
	checkforceclose(m_logFd);
}

cmstring GetFooter()
{
        return mstring("<hr><address>Server: ") + cfg::agentname
                + "&nbsp;&nbsp;|&nbsp;&nbsp;<a href=\"https://flattr.com/thing/51105/Apt-Cacher-NG\">Flattr this!"
                "</a>&nbsp;&nbsp;|&nbsp;&nbsp;<a href=\"http://www.unix-ag.uni-kl.de/~bloch/acng/\">Apt-Cacher NG homepage</a></address>";
}

/*
 *  TODO: this is kept in expiration class for historical reasons. Should be moved to some shared upper
 * class, like "detachedtask" or something like that
 */
void tSpecOpDetachable::Run()
{

	if (m_parms.cmd.find("&sigabort")!=stmiss)
	{
		lockguard g(g_StateCv);
		g_sigTaskAbort=true;
		g_StateCv.notifyAll();
		tStrPos nQuest=m_parms.cmd.find("?");
		if(nQuest!=stmiss)
		{
			tSS buf(255);
			buf << "HTTP/1.1 302 Redirect\r\nLocation: "
				<< m_parms.cmd.substr(0,nQuest)<< "\r\nConnection: close\r\n\r\n";
			SendRawData(buf.data(), buf.size(), 0);
		}
		return;
	}

	SendChunkedPageHeader("200 OK", "text/html");

	tSS deco;
	const char *mark(nullptr);
	if(m_szDecoFile &&
			( deco.initFromFile((cfg::confdir+SZPATHSEP+m_szDecoFile).c_str())
					|| (!cfg::suppdir.empty() &&
							deco.initFromFile((cfg::suppdir+SZPATHSEP+m_szDecoFile).c_str()))))
	{
		mark=::strchr(deco.rptr(), '~');
		if(mark)
		{
			// send fancy header only to the remote caller
			SendChunkRemoteOnly(deco.rptr(), mark-deco.rptr());
			deco.drop((mark-deco.rptr())+1);
		}
		else
		{
			// send fancy header only to the remote caller
			SendChunkRemoteOnly(deco.c_str(), deco.size());
			deco.clear();
		}
	}

	tSS logPath;

	time_t other_id=0;

	{ // this is locked just to make sure that only one can register as master
		lockguard guard(g_StateCv);
		if(0 == nBgTimestamp) // ok, not running yet -> become the log source then
		{
			auto id = time(0);

			logPath.clear();
			logPath<<cfg::logdir<<CPATHSEP<< MAINT_PFX << id << ".log.html";
			m_reportStream.open(logPath.c_str(), ios::out);
			if(m_reportStream.is_open())
			{
				m_reportStream << LOG_DECO_START;
				m_reportStream.flush();
				nBgTimestamp = id;
			}
			else
			{
				nBgTimestamp = 0;
				SendChunk("Failed to create the log file, aborting.");
				return;
			}
		}
		else other_id = nBgTimestamp;
	}

	if(other_id)
	{
		SendChunkSZ("<font color=\"blue\">A maintenance task is already running!</font>\n");
		SendFmtRemote << " (<a href=\"" << m_parms.cmd << "&sigabort=" << rand()
				<< "\">Cancel</a>)";
		SendChunkSZ("<br>Attempting to attach to the log output... <br>\n");

		tSS sendbuf(4096);
		lockuniq g(g_StateCv);

		for(;;)
		{
			if(other_id != nBgTimestamp)
			{
				// task is gone or replaced by another?
				SendChunkSZ("<br><b>End of log output. Please reload to run again.</b>\n");
				goto finish_action;
			}
			if(m_logFd < 0)
			{
				logPath.clear();
				logPath<<cfg::logdir<<CPATHSEP<<MAINT_PFX << other_id << ".log.html";
				m_logFd=open(logPath.c_str(), O_RDONLY|O_NONBLOCK);

				if(m_logFd>=0)
					SendChunk("ok:<br>\n");
				else
				{
					SendChunk("Failed to open log output, please retry later.<br>\n");
					goto finish_action;
				}

			}
			while(true)
			{
				int r = sendbuf.sysread(m_logFd);
				if(r < 0)
					goto finish_action;
				if(r == 0)
				{
					g_StateCv.wait_for(g, 10, 1);
					break;
				}
				SendChunkRemoteOnly(sendbuf.rptr(), sendbuf.size());
				sendbuf.clear();
			}
		}
		// unreachable
		goto finish_action;
	}
	else
	{
			/*****************************************************
			 * This is the worker part
			 *****************************************************/
			lockuniq g(&g_StateCv);
			g_sigTaskAbort=false;
			ACTION_ON_LEAVING(cleaner,[&](){
				g.reLockSafe();
				nBgTimestamp = 0; g_StateCv.notifyAll();
			})

			g.unLock();

			SendFmt << "Maintenance task <b>" << GetTaskName()
					<< "</b>, apt-cacher-ng version: " ACVERSION;
			string link = "http://" + GetHostname() + ":" + cfg::port + "/" + cfg::reportpage;
			SendFmtRemote << " (<a href=\"" << m_parms.cmd << "&sigabort=" << rand()
					<< "\">Cancel</a>)"
					<< "\n<!--\n"
					<< maark << int(ControLineType::BeforeError)
					<< "Maintenance Task: " << GetTaskName() << "\n"
					<< maark << int(ControLineType::BeforeError)
					<< "See file " << logPath << " for more details.\n"
					<< maark << int(ControLineType::BeforeError)
					<< "Server control address: " << link
					<< "\n-->\n";
			string xlink = "<br>\nServer link: <a href=\"" + link + "\">" + link + "</a><br>\n";
			SendChunkLocalOnly(xlink.data(), xlink.size());
			SendFmt << "<br>\n";
			SendChunkRemoteOnly(WITHLEN("<form id=\"mainForm\" action=\"#top\">\n"));

			Action();


			if (!m_pathMemory.empty())
			{
				bool unchint=false;
				bool ehprinted=false;

				for(const auto& err: m_pathMemory)
				{
					if(err.second.msg.empty())
						continue;

					if(!ehprinted)
					{
						ehprinted=true;
						SendChunkRemoteOnly(WITHLEN(
								"<br><b>Error summary:</b><br>"));
					}

					unchint = unchint
							||endsWithSzAr(err.first, "Packages")
							||endsWithSzAr(err.first, "Sources");

					SendFmtRemote << err.first << ": <label>"
							<< err.second.msg
							<<  "<input type=\"checkbox\" name=\"kf\" value=\""
							<< tSS::hex << err.second.id << tSS::dec
							<< "\"</label>" << hendl;
				}

				if(unchint)
				{
					SendChunkRemoteOnly(WITHLEN(
					"<i>Note: some uncompressed index versions like Packages and Sources are no"
					" longer offered by most mirrors and can be safely removed if a compressed version exists.</i>\n"
							));
				}

				SendChunkRemoteOnly(WITHLEN(
				"<br><b>Action(s):</b><br>"
					"<input type=\"submit\" name=\"doDelete\""
					" value=\"Delete selected files\">"
					"|<input type=\"submit\" name=\"doTruncate\""
											" value=\"Truncate selected files to zero size\">"
					"|<button type=\"button\" onclick=\"checkOrUncheck(true);\">Check all</button>"
					"<button type=\"button\" onclick=\"checkOrUncheck(false);\">Uncheck all</button><br>"));
				auto blob=BuildCompressedDelFileCatalog();
				SendChunkRemoteOnly(blob.data(), blob.size());
			}

			SendFmtRemote << "<br>\n<a href=\"/"<< cfg::reportpage<<"\">Return to main page</a>"
					"</form>";
			auto& f(GetFooter());
						SendChunkRemoteOnly(f.data(), f.size());

	}

	finish_action:

	if(!deco.empty())
		SendChunkRemoteOnly(deco.c_str(), deco.size());
}

bool tSpecOpDetachable::CheckStopSignal()
{
	lockguard g(&g_StateCv);
	return g_sigTaskAbort;
}

void tSpecOpDetachable::DumpLog(time_t id)
{
	filereader reader;

	if (id<=0)
		return;

	tSS path(cfg::logdir.length()+24);
	path<<cfg::logdir<<CPATHSEP<<MAINT_PFX << id << ".log.html";
	if (!reader.OpenFile(path))
		SendChunkRemoteOnly(WITHLEN("Log not available"));
	else
		SendChunkRemoteOnly(reader.GetBuffer(), reader.GetSize());
}

void tSpecOpDetachable::SendChunkLocalOnly(const char *data, size_t len)
{
	if(m_reportStream.is_open())
	{
		m_reportStream.write(data, len);
		m_reportStream.flush();
		g_StateCv.notifyAll();
	}
}

time_t tSpecOpDetachable::GetTaskId()
{
	lockguard guard(&g_StateCv);
	return nBgTimestamp;
}

#ifdef HAVE_ZLIB
mstring tSpecOpDetachable::BuildCompressedDelFileCatalog()
{
	mstring ret;
	tSS buf;
	//auto hm = m_delCboxFilter.size();
	for(const auto& kv: m_pathMemory)
	{
		unsigned len=kv.first.size();
		buf.add((const char*) &kv.second.id, sizeof(kv.second.id))
		.add((const char*) &len, sizeof(len))
		.add(kv.first.data(), kv.first.length());
	}
	unsigned uncompSize=buf.size();
	tSS gzBuf;
	uLongf gzSize = compressBound(buf.size())+32; // extra space for length header
	gzBuf.setsize(gzSize);
	// length header
	gzBuf.add((const char*)&uncompSize, sizeof(uncompSize));
	if(Z_OK == compress((Bytef*) gzBuf.wptr(), &gzSize,
			(const Bytef*)buf.rptr(), buf.size()))
	{
		ret = "<input type=\"hidden\" name=\"blob\" value=\"";
//		ret += BytesToHexString((const uint8_t*) gzBuf.wptr(), (unsigned short) gzSize);
		ret += EncodeBase64(gzBuf.rptr(), (unsigned short) gzSize+sizeof(uncompSize));
		ret += "\">";
		return ret;
	}
	return "";
}

#endif

#ifdef DEBUG
void tBgTester::Action()
{
	for (int i = 0; i < 10 && !CheckStopSignal(); i++, sleep(1))
	{
		char buf[1024];
		time_t t;
		struct tm *tmp;
		t = time(nullptr);
		tmp = localtime(&t);
		strftime(buf, sizeof(buf), "%c", tmp);
		SendFmt << buf << "<br>\n";
	}
}

#endif // DEBUG

}
