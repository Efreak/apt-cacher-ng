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

using namespace std;
using namespace SMARTPTR_SPACE;

#define LOG_DECO_START "<html><head><style type=\"text/css\">" \
".WARNING { color: orange; }\n.ERROR { color: red; }\n" \
"</style></head><body>"
#define LOG_DECO_END "</body></html>"

bool bSigTaskAbort=false;
pthread_mutex_t abortMx=PTHREAD_MUTEX_INITIALIZER;

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
SMARTPTR_SPACE::weak_ptr<tSpecOpDetachable::tProgressTracker> tSpecOpDetachable::g_pTracker;

cmstring& GetFooter()
{
	static mstring footer;
	if(footer.empty())
	{
		footer = string("<hr><address>Server: ") + acfg::agentname +
		"&nbsp;&nbsp;|&nbsp;&nbsp;<a href=\"https://flattr.com/thing/51105/Apt-Cacher-NG\">"
    "<!-- http://api.flattr.com/button/flattr-badge-large.png on 2015-04-06 -->"
			"<img border=0  style='height:15px;margin-bottom:-4px;margin-right:-1px;' alt=\"Flattr this!\" "
    " src=\"data:image/png;base64,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\"/>"
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
		lockguard g(&abortMx);
		bSigTaskAbort=true;
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
	const char *mark(NULL);
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

	tSS msg;

	tProgTrackPtr pTracked;

	{ // this is locked just to make sure that only one can register as master
		lockguard guard(abortMx);
		pTracked=g_pTracker.lock();
		if(!pTracked) // ok, not running yet -> become the log source then
		{
			m_pTracker = make_shared<tProgressTracker>();
			msg.clear();
			time_t nMaintId=time(0);

			msg<<acfg::logdir<<CPATHSEP<< MAINT_PFX << nMaintId << ".log.html";
			m_reportStream.open(msg.c_str(), ios::out);
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
			lockguard g(*pTracked);

			if(!pTracked->id && pTracked->nEndSize==off_t(-1))
				break; // error? source is gone but end mark not set?
			// otherwise: end known OR sorce active

			if(nSent==pTracked->nEndSize)
				break; // DONE!

			if(!fd)
			{
				msg<<acfg::logdir<<CPATHSEP<<MAINT_PFX << pTracked->id << ".log.html";
				fd=open(msg.c_str(), O_RDONLY);
				msg.clear();
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

			msg.sysread(fd); // can be more than tracker reported. checks need to consider this
			SendChunk(msg.data(), msg.length());
			nSent+=msg.length();
			msg.clear();

			/* when file is truncated (disk full), the end size might be unreachable
			 * -> detect the time out
			 */
			if(pTracked->wait_until(GetTime()+33, 1))
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
				lockguard g(&abortMx);
				bSigTaskAbort=false;
			}

			SendFmt << "Maintenance task <b>" << GetTaskName()
					<< "</b>, apt-cacher-ng version: " ACVERSION;
			SendFmtRemote << " (<a href=\"" << m_parms.cmd << "&sigabort=" << rand()
					<< "\">Cancel</a>)";
			SendFmt << "<br>\n";
			SendChunkRemoteOnly(WITHLEN("<form id=\"mainForm\" action=\"#top\">\n"));

			Action();

			if (!m_delCboxFilter.empty())
				SendChunkRemoteOnly(WITHLEN(
				"<br><b>Action(s):</b><br>"
					"<input type=\"submit\" name=\"doDelete\""
					" value=\"Delete selected files\">"
					"|<button type=\"button\" onclick=\"checkOrUncheck(true);\">Check all</button>"
					"<button type=\"button\" onclick=\"checkOrUncheck(false);\">Uncheck all</button><br>"));


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
	lockguard g(&abortMx);
	return bSigTaskAbort;
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

void tSpecOpDetachable::AfterSendChunk(const char *data, size_t len)
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
	lockguard guard(&abortMx);
	tProgTrackPtr pTracked = g_pTracker.lock();
	return pTracked ? pTracked->id : 0;
}

#ifdef DEBUG
void tBgTester::Action()
{
	for (int i = 0; i < 10 && !CheckStopSignal(); i++, sleep(1))
	{
		char buf[1024];
		time_t t;
		struct tm *tmp;
		t = time(NULL);
		tmp = localtime(&t);
		strftime(buf, sizeof(buf), "%c", tmp);
		SendFmt << buf << "<br>\n";
	}
}
#endif // DEBUG
