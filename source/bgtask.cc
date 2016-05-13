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

bool tSpecOpDetachable::g_sigTaskAbort=false;
acmutex tSpecOpDetachable::g_abortMx;


tSpecOpDetachable::~tSpecOpDetachable()
{
	if(m_pTracker)
		m_pTracker->SetEnd(m_reportStream.is_open() ? off_t(m_reportStream.tellp()) : off_t(0));

	if(m_reportStream.is_open())
	{
		m_reportStream << LOG_DECO_END;
		m_reportStream.close();
	}
}

// the obligatory definition of static members :-(
WEAK_PTR<tSpecOpDetachable::tProgressTracker> tSpecOpDetachable::g_pTracker;

cmstring& GetFooter()
{
	static mstring footer;
	if(footer.empty())
	{
		footer = string("<hr><address>Server: ") + acfg::agentname +
		"&nbsp;&nbsp;|&nbsp;&nbsp;<a href=\"https://flattr.com/thing/51105/Apt-Cacher-NG\">Flattr this!"
    "</a>&nbsp;&nbsp;|&nbsp;&nbsp;<a href=\"http://www.unix-ag.uni-kl.de/~bloch/acng/\">Apt-Cacher NG homepage</a></address>";
	}
	return footer;
}

/*
 *  TODO: this is kept in expiration class for historical reasons. Should be moved to some shared upper
 * class, like "detachedtask" or something like that
 */
void tSpecOpDetachable::Run()
{

	if (m_parms.cmd.find("&sigabort")!=stmiss)
	{
		lockguard g(&g_abortMx);
		g_sigTaskAbort=true;
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
			( deco.initFromFile((acfg::confdir+SZPATHSEP+m_szDecoFile).c_str())
					|| (!acfg::suppdir.empty() &&
							deco.initFromFile((acfg::suppdir+SZPATHSEP+m_szDecoFile).c_str()))))
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

	tProgTrackPtr pTracked;

	{ // this is locked just to make sure that only one can register as master
		lockguard guard(&g_abortMx);
		pTracked=g_pTracker.lock();
		if(!pTracked) // ok, not running yet -> become the log source then
		{
			m_pTracker = make_shared<tProgressTracker>();
			time_t nMaintId=time(0);

			logPath.clear();
			logPath<<acfg::logdir<<CPATHSEP<< MAINT_PFX << nMaintId << ".log.html";
			m_reportStream.open(logPath.c_str(), ios::out);
			if(m_reportStream.is_open())
			{
				m_reportStream << LOG_DECO_START;
				m_reportStream.flush();
				m_pTracker->id = nMaintId;
				g_pTracker=m_pTracker;
			}
			else
			{
				SendChunk("Failed to create the log file, aborting.");
				return;
			}
		}
	}

	if(pTracked)
	{
		SendChunkSZ("<font color=\"blue\">A maintenance task is already running!</font>\n");
		SendFmtRemote << " (<a href=\"" << m_parms.cmd << "&sigabort=" << rand()
				<< "\">Cancel</a>)";
		SendChunkSZ("<br>Attempting to attach to the log output... <br>\n");

		off_t nSent(0);
		int fd(0);
		acbuf xx;

		for(;;)
		{
			lockuniq g(*pTracked);

			if(!pTracked->id && pTracked->nEndSize==off_t(-1))
				break; // error? source is gone but end mark not set?
			// otherwise: end known OR sorce active

			if(nSent==pTracked->nEndSize)
				break; // DONE!

			if(!fd)
			{
				logPath.clear();
				logPath<<acfg::logdir<<CPATHSEP<<MAINT_PFX << pTracked->id << ".log.html";
				fd=open(logPath.c_str(), O_RDONLY);

				if(fd>=0)
				{
					set_nb(fd);
					SendChunk("ok:<br>\n");
				}
				else
				{
					SendChunk("Failed to open log output, please retry later.<br>\n");
					goto finish_action;
				}

			}
			tSS msg;
			msg.sysread(fd); // can be more than tracker reported. checks need to consider this
			SendChunk(msg.data(), msg.length());
			nSent+=msg.length();
			msg.clear();

			/* when file is truncated (disk full), the end size might be unreachable
			 * -> detect the time out
			 */
			if(pTracked->wait_for(g, 33, 1))
				break;
		}
		checkforceclose(fd);
		goto finish_action;
	}
	else
	{
			/*****************************************************
			 * This is the worker part
			 *****************************************************/
			{
				lockguard g(&g_abortMx);
				g_sigTaskAbort=false;
			}

			SendFmt << "Maintenance task <b>" << GetTaskName()
					<< "</b>, apt-cacher-ng version: " ACVERSION;
			string link = "http://" + GetHostname() + ":" + acfg::port + "/" + m_parms.cmd;
			SendFmtRemote << " (<a href=\"" << m_parms.cmd << "&sigabort=" << rand()
					<< "\">Cancel</a>)"
					<< "\n<!--\n"
					<< maark << int(ControLineType::BeforeError)
					<< "Maintenance Task: " << GetTaskName() << "\n"
					<< maark << int(ControLineType::BeforeError)
					<< "See file " << logPath << " for more details.\n"
					<< maark << int(ControLineType::BeforeError)
					<< "Server url: " << link
					<< "\n-->\n"
//					<< "<!--\n" << maark << int(ControLineType::Error) << "test\n-->\n"
					;
			string xlink = "<br>\nServer link: <a href=\"" + link + "\">" + link + "</a><br>\n";
			SendChunkLocalOnly(xlink.data(), xlink.size());
			SendFmt << "<br>\n";
			SendChunkRemoteOnly(WITHLEN("<form id=\"mainForm\" action=\"#top\">\n"));

			Action();

			if (!m_delCboxFilter.empty())
			{
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

			SendFmtRemote << "<br>\n<a href=\"/"<< acfg::reportpage<<"\">Return to main page</a>"
					"</form>";
			auto& f(GetFooter());
						SendChunkRemoteOnly(f.data(), f.size());

	}

	finish_action:

	if(!deco.empty())
		SendChunkRemoteOnly(deco.c_str(), deco.size());
}

void tSpecOpDetachable::tProgressTracker::SetEnd(off_t endSize)
{
	lockguard g(this);
	//lockguard g2(&abortMx); // the master may go out of scope, protect weak_ptr
	nEndSize=endSize;
	if(time(0) == id)
		::sleep(1); // just to make sure that the next id good
	id=0;
	notifyAll();
}

bool tSpecOpDetachable::CheckStopSignal()
{
	lockguard g(&g_abortMx);
	return g_sigTaskAbort;
}

void tSpecOpDetachable::DumpLog(time_t id)
{
	filereader reader;

	if (id<=0)
		return;

	tSS path(acfg::logdir.length()+24);
	path<<acfg::logdir<<CPATHSEP<<MAINT_PFX << id << ".log.html";
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
		if(m_pTracker)
			m_pTracker->notifyAll();
	}
}

time_t tSpecOpDetachable::GetTaskId()
{
	lockguard guard(&g_abortMx);
	tProgTrackPtr pTracked = g_pTracker.lock();
	return pTracked ? pTracked->id : 0;
}

#ifdef HAVE_ZLIB
mstring tSpecOpDetachable::BuildCompressedDelFileCatalog()
{
	mstring ret;
	tSS buf;
	//auto hm = m_delCboxFilter.size();
	for(const auto& kv: m_delCboxFilter)
	{
		unsigned len=kv.first.size();
		buf.add((const char*) &kv.second, sizeof(kv.second))
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
