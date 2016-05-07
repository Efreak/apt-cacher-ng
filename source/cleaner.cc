/*
 * cleaner.cc
 *
 *  Created on: 05.02.2011
 *      Author: ed
 */

#include "debug.h"
#include "meta.h"

#include "cleaner.h"

#include "fileitem.h"
#include "acfg.h"
#include "caddrinfo.h"
#include "tcpconnect.h"

#include <limits>
#include <cstring>
using namespace std;

#define TERM_VAL (time_t(-1))

cleaner::cleaner() : m_thr(0)
{
	Init();
}
void cleaner::Init()
{
	for(auto&ts : stamps)
		ts=END_OF_TIME;
}

cleaner::~cleaner()
{
}

void cleaner::WorkLoop()
{
	LOGSTART("cleaner::WorkLoop");

	lockuniq g(this);
	for(;;)
	{
		eType what = TYPE_EXDNS;
		time_t when = END_OF_TIME;

		if(m_terminating)
			return;

		// ok, who's next?
		for (unsigned i = 0; i < ETYPE_MAX; ++i)
		{
			if (stamps[i] < when)
			{
				what = (eType) i;
				when = stamps[i];
			}
		}

		auto now=GetTime();
		if(when > now)
		{
			// work around buggy STL, add some years on top and hope it will be fixed everywhere by then
			wait_until(g, when == END_OF_TIME ? (now | 0x3ffffffe) : when, 111);
			continue;
		}
		stamps[what] = END_OF_TIME;
		g.unLock();

		// good, do the work now
		time_t time_nextcand=END_OF_TIME;
		switch(what)
		{
		case TYPE_ACFGHOOKS:
			time_nextcand = acfg::BackgroundCleanup();
			USRDBG("acfg::ExecutePostponed, nextRunTime now: " << time_nextcand);
			break;

		case TYPE_EXCONNS:
			time_nextcand = g_tcp_con_factory.BackgroundCleanup();
			USRDBG("tcpconnect::ExpireCache, nextRunTime now: " << time_nextcand);
			break;

		case TYPE_EXDNS:
			time_nextcand = CAddrInfo::BackgroundCleanup();
			USRDBG("CAddrInfo::ExpireCache, nextRunTime now: " << time_nextcand);
			break;

		case TYPE_EXFILEITEM:
			time_nextcand = fileItemMgmt::BackgroundCleanup();
			USRDBG("fileitem::DoDelayedUnregAndCheck, nextRunTime now: " << time_nextcand);
			break;

		case ETYPE_MAX:
			return; // heh?
		}

		if(time_nextcand <= now || time_nextcand < 1)
		{
			aclog::err(tSS() << "ERROR: looping bug candidate on " << what
					<< ", value: " << time_nextcand);
			time_nextcand=GetTime()+60;
		}

		g.reLock();

		if (time_nextcand < stamps[what])
			stamps[what] = time_nextcand;
	};
}

inline void * CleanerThreadAction(void *pVoid)
{
	static_cast<cleaner*>(pVoid)->WorkLoop();
	return nullptr;
}

void cleaner::ScheduleFor(time_t when, eType what)
{
	setLockGuard;
	if(m_thr == 0)
	{
		Init();
		stamps[what] = when;
		pthread_create(&m_thr, nullptr, CleanerThreadAction, (void *)this);
	}
	else
	{
		// already scheduled for an earlier moment or
		// something else is pointed and this will come earlier anyhow

		if(when > stamps[what])
			return;

		stamps[what] = when;
		notifyAll();
	}
}

void cleaner::Stop()
{
	{
		setLockGuard;

		if(!m_thr)
			return;

		m_terminating = true;
		notifyAll();
	}
    pthread_join(m_thr, nullptr);

    setLockGuard;
    m_thr = 0;
}

void cleaner::dump_status()
{
	setLockGuard;
	tSS msg;
	msg << "Cleanup schedule:\n";
	for(int i=0; i<cleaner::ETYPE_MAX; ++i)
		msg << stamps[i] << " (in " << (stamps[i]-GetTime()) << " seconds)\n";
	aclog::err(msg);
}


void dump_handler(int) {
	fileItemMgmt::dump_status();
	g_victor.dump_status();
	g_tcp_con_factory.dump_status();
	acfg::dump_trace();
}

