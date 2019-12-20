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

namespace acng
{

cleaner::cleaner(bool noop) : m_thr(0), m_noop(noop)
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
		// XXX: review the flow, is this always safe?
		eType what = TYPE_EXCONNS;
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
			// work around buggy STL: add some years on top and hope it will be fixed then
			if(when == END_OF_TIME)
				when = now | 0x3ffffffe;
			wait_until(g, when, 111);
			continue;
		}
		stamps[what] = END_OF_TIME;
		g.unLock();

		// good, do the work now
		time_t time_nextcand=END_OF_TIME;
		switch(what)
		{
		case TYPE_ACFGHOOKS:
			time_nextcand = cfg::BackgroundCleanup();
			USRDBG("acng::cfg:ExecutePostponed, nextRunTime now: " << time_nextcand);
			break;

		case TYPE_EXCONNS:
			time_nextcand = g_tcp_con_factory.BackgroundCleanup();
			USRDBG("tcpconnect::ExpireCache, nextRunTime now: " << time_nextcand);
			break;
		case TYPE_EXFILEITEM:
			time_nextcand = TFileItemUser::BackgroundCleanup();
			USRDBG("fileitem::DoDelayedUnregAndCheck, nextRunTime now: " << time_nextcand);
			break;

		case ETYPE_MAX:
			return; // heh?
		}

		if(time_nextcand <= now || time_nextcand < 1)
		{
			log::err(tSS() << "ERROR: looping bug candidate on " << (int) what
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
	if(m_noop) return;

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
	log::err(msg);
}


void ACNG_API dump_handler(evutil_socket_t fd, short what, void *arg) {
	TFileItemUser::dump_status();
	cleaner::GetInstance().dump_status();
	g_tcp_con_factory.dump_status();
	cfg::dump_trace();
}

cleaner& cleaner::GetInstance(bool initAsNoop)
{
	static cleaner g_victor(initAsNoop);
	return g_victor;
}

}
