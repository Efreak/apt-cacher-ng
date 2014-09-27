
#include "meta.h"

#include <string.h>
#include <errno.h>

#include "debug.h"

#include "aclogger.h"
#include "acfg.h"
#include "lockable.h"
#include "filereader.h"

#include "fileio.h"
#include <unistd.h>

#include <vector>
#include <deque>
#include <string.h>
#include <iostream>
#include <fstream>

using namespace std;

namespace aclog
{

ofstream fErr, fStat;
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;

bool open()
{
	// only called in the beginning or when reopening, already locked...
	// lockguard g(&mx);

	if(acfg::logdir.empty())
		return true;
	
	string apath(acfg::logdir+"/apt-cacher.log"), epath(acfg::logdir+"/apt-cacher.err");
	
	if(fErr.is_open())
		fErr.close();
	if(fStat.is_open())
		fStat.close();

	fErr.open(epath.c_str(), ios::out | ios::app);
	fStat.open(apath.c_str(), ios::out | ios::app);

	return fStat.is_open() && fErr.is_open();
}


void transfer(char cLogType, uint64_t nCount, const char *szClient, const char *szPath)
{
	lockguard g(&mx);
	if(!fStat.is_open())
		return;
	fStat << time(0) << '|' << cLogType << '|' << nCount;
	if(acfg::verboselog)
		fStat << '|' << szClient << '|' << szPath;
	fStat << '\n';
	if(acfg::debug&LOG_FLUSH) fStat.flush();
}

void misc(const string & sLine, const char cLogType)
{
	lockguard g(&mx);
	if(!fStat.is_open())
		return;

	fStat << time(0) << '|' << cLogType << '|' << sLine << '\n';
	
	if(acfg::debug&LOG_FLUSH)
		fStat.flush();
}

void err(const char *msg, const char *client)
{
	lockguard g(&mx);

	if(!fErr.is_open())
	{
/*#ifdef DEBUG
		cerr << msg <<endl;
#endif
*/
		return;
	}
	
	static char buf[32];
	const time_t tm=time(NULL);
	ctime_r(&tm, buf);
	buf[24]=0;
	fErr << buf << '|';
	if(client)
		fErr << client << ": ";
	fErr << msg << '\n';

#ifdef DEBUG
	if(acfg::debug & LOG_DEBUG)
		cerr << buf << msg <<endl;
#endif

	if(acfg::debug & LOG_DEBUG)
		fErr.flush();
}

void flush()
{
	lockguard g(&mx);
	if(fErr.is_open()) fErr.flush();
	if(fStat.is_open()) fStat.flush();
}

void close(bool bReopen)
{
	lockguard g(&mx);
	if(acfg::debug>=LOG_MORE) cerr << (bReopen ? "Reopening logs...\n" : "Closing logs...\n");
	fErr.close();
	fStat.close();
	if(bReopen)
		aclog::open();
}


#define DAYSECONDS (3600*24)
#define WEEKSECONDS (DAYSECONDS * 7)

#ifndef MINIBUILD
inline deque<tRowData> GetStats()
{
	string sDataFile=acfg::cachedir+SZPATHSEP"_stats_dat";
	deque<tRowData> out;
	
	time_t now = time(NULL);

	for (int i = 0; i < 7; i++)
	{
		tRowData d;
		d.to = now - i * DAYSECONDS;
		d.from = d.to - DAYSECONDS;
		out.push_back(d);
	}

	for (auto& log : ExpandFilePattern(acfg::logdir + SZPATHSEP "apt-cacher*.log", false))
	{
		if (acfg::debug >= LOG_MORE)
			cerr << "Reading log file: " << log << endl;
		filereader reader;
		if (!reader.OpenFile(log))
		{
			aclog::err("Error opening a log file");
			continue;
		}
		string sLine;
		tStrVec tokens;

		while (reader.GetOneLine(sLine))
		{
			// cout << "got line: " << sLine <<endl;
			tokens.clear();
			if (Tokenize(sLine, "|", tokens) < 3)
				continue;

			// cout << "having: " << tokens[0] << ", " << tokens[1] << ", " << tokens[2]<<endl;

			time_t when = strtoul(tokens[0].c_str(), 0, 10);
			if (when > out.front().to || when < out.back().from)
				continue;

			for (auto it = out.rbegin(); it != out.rend(); it++)
			{
				if (when < it->from || when > it->to)
					continue;

				unsigned long dcount = strtoul(tokens[2].c_str(), 0, 10);
				switch (*tokens[1].c_str())
				{
				case ('I'):
					it->byteIn += dcount;
					it->reqIn++;
					break;
				case ('O'):
					it->byteOut += dcount;
					it->reqOut++;
					break;
				default:
					continue;
				}
			}
		}
	}
	return out;
}


string GetStatReport()
{
	string ret;
	vector<char> buf(1024);
	for (auto& entry : aclog::GetStats())
	{
		auto reqMax = std::max(entry.reqIn, entry.reqOut);
		auto dataMax = std::max(entry.byteIn, entry.byteOut);

		if (0 == dataMax || 0 == reqMax)
			continue;

		char tbuf[50];
		size_t zlen(0);
		ctime_r(&entry.from, tbuf);
		struct tm *tmp = localtime(&entry.from);
		if (!tmp)
			goto time_error;
		zlen = strftime(tbuf, sizeof(tbuf), TIMEFORMAT, tmp);
		if (!zlen)
			goto time_error;

		if (entry.from != entry.to)
		{
			tmp = localtime(&entry.to);
			if (!tmp)
				goto time_error;
			if (0 == strftime(tbuf + zlen, sizeof(tbuf) - zlen,
					" - " TIMEFORMAT, tmp))
				goto time_error;
		}
		snprintf(&buf[0], buf.size(), "<tr bgcolor=\"white\">"

			"<td class=\"colcont\">%s</td>"

			"<td class=\"coltitle\"><span>&nbsp;</span></td>"

			"<td class=\"colcont\">%lu (%2.2f%%)</td>"
			"<td class=\"colcont\">%lu (%2.2f%%)</td>"
			"<td class=\"colcont\">%lu</td>"

			"<td class=\"coltitle\"><span>&nbsp;</span></td>"

			"<td class=\"colcont\">%2.2f MiB (%2.2f%%)</td>"
			"<td class=\"colcont\">%2.2f MiB (%2.2f%%)</td>"
			"<td class=\"colcont\">%2.2f MiB</td>"
			"</tr>", tbuf, reqMax - entry.reqIn, double(reqMax - entry.reqIn)
				/ reqMax * 100, // hitcount
				entry.reqIn, double(entry.reqIn) / reqMax * 100, // misscount
				reqMax,

				double(dataMax - entry.byteIn) / 1048576, double(dataMax
						- entry.byteIn) / dataMax * 100, // hitdata
				double(entry.byteIn) / 1048576, double(entry.byteIn) / dataMax
						* 100, // missdata
				double(dataMax) / 1048576

		); //, int(entry.ratioSent*nRatWid), int((1.0-entry.ratioSent)*nRatWid));
		ret += &buf[0];
		continue;
		time_error: ret
				+= " Invalid time value detected, check the stats database. ";
	}
	return ret;
}
#endif


}



tErrnoFmter::tErrnoFmter(const char *prefix)
{
	char buf[32];
	buf[0]=buf[31]=0x0;

	if(prefix)
		assign(prefix);

#ifdef _GNU_SOURCE
//#warning COMPILER BUG DETECTED -- _GNU_SOURCE was preset in C++ mode
	append(strerror_r(errno, buf, sizeof(buf)-1));
#else
	int err=errno;
	if(strerror_r(err, buf, sizeof(buf)-1))
		append(tSS() << "UNKNOWN ERROR: " << err);
	else
		append(buf);
#endif
}

#ifdef DEBUG

static class : public lockable, public std::map<pthread_t, int>
{} stackDepths;

t_logger::t_logger(const char *szFuncName,  const void * ptr)
{
	m_id = pthread_self();
	m_szName = szFuncName;
	callobj = uintptr_t(ptr);
	{
		lockguard __lockguard(stackDepths);
		m_nLevel = stackDepths[m_id]++;
	}
	// writing to the level of parent since it's being "created there"
	GetFmter() << ">> " << szFuncName << " [T:"<<m_id<<" P:0x"<< tSS::hex<< callobj << tSS::dec <<"]";
	Write();
	m_nLevel++;
}

t_logger::~t_logger()
{
	m_nLevel--;
	GetFmter() << "<< " << m_szName << " [T:"<<m_id<<" P:0x"<< tSS::hex<< callobj << tSS::dec <<"]";
	Write();
	lockguard __lockguard(stackDepths);
	stackDepths[m_id]--;
	if(0 == stackDepths[m_id])
		stackDepths.erase(m_id);
}

tSS & t_logger::GetFmter()
{
	m_strm.clear();
	for(uint i=0;i<m_nLevel;i++)
		m_strm << "\t";
	m_strm<< " - ";
	return m_strm;
}

void t_logger::Write(const char *pFile, unsigned int nLine)
{
	if(pFile)
	{
		const char *p=strrchr(pFile, CPATHSEP);
		pFile=p?(p+1):pFile;
		m_strm << " [T:" << m_id << " S:" << pFile << ":" << tSS::dec << nLine
				<<" P:0x"<< tSS::hex<< callobj << tSS::dec <<"]";
	}
	aclog::err(m_strm.c_str());
}

#endif
