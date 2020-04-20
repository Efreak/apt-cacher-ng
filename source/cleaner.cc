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

// forward declation, see caddrinfo.cc
time_t expireDnsCache();

cleaner::cleaner(bool noop) : m_thr(0), m_noop(noop)
{
	Init();
}
void cleaner::Init()
{
	stamps.fill(END_OF_TIME);
}

cleaner::~cleaner()
{
}

void cleaner::WorkLoop()
{
	LOGSTART("cleaner::WorkLoop");

	while(true)
	{
		if (m_terminating || g_global_shutdown)
			return;

		decltype(stamps) snapshot;
		auto now = GetTime();
		{
			lockuniq g(this);
			snapshot = stamps;
			stamps.fill(END_OF_TIME);
		}
		for (unsigned i = 0; i < ETYPE_MAX; ++i)
		{
			auto &time_cand = snapshot[i];
			if (time_cand > now)
				continue;
			if (m_terminating || g_global_shutdown)
				return;

			switch (eType(i))
			{
			case TYPE_ACFGHOOKS:
				time_cand = cfg::BackgroundCleanup();
				USRDBG("acng::cfg:ExecutePostponed, nextRunTime now: " << time_cand)
				;
				break;

			case TYPE_EXCONNS:
				time_cand = g_tcp_con_factory.BackgroundCleanup();
				USRDBG("tcpconnect::ExpireCache, nextRunTime now: " << time_cand);
				break;
			case TYPE_EXFILEITEM:
				time_cand = fileItemMgmt::BackgroundCleanup();
				USRDBG("fileitem::DoDelayedUnregAndCheck, nextRunTime now: " << time_cand);
				break;
			case DNS_CACHE:
				time_cand = expireDnsCache();
				USRDBG("dnsExpiration, nextRunTime now: " << time_cand);
				break;
			case ETYPE_MAX:
				return; // heh?
			}
		}
		// playback the calculated results and calculate the delay

		lockuniq g(this);
		now = GetTime();
		time_t next = END_OF_TIME;
		for (unsigned i = 0; i < ETYPE_MAX; ++i)
		{
			auto t = std::min(snapshot[i], stamps[i]);
			next = std::min(next, t);
			stamps[i] = t;
		}
		if (next <= now)
			continue;
		auto delta = next - now;
		// limit this to a day to avoid buggy STL behavior reported in the past
		wait_for(g, std::min(long(delta), long(84600)), 1);
	}
}

inline void * CleanerThreadAction(void *pVoid)
{
	static_cast<cleaner*>(pVoid)->WorkLoop();
	return nullptr;
}

void cleaner::ScheduleFor(time_t when, eType what)
{
	if(m_noop || g_global_shutdown)
		return;

	setLockGuard;
	if(m_thr == 0)
	{
		if(g_global_shutdown)
			return;
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
	LOGSTARTFUNC;

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
	fileItemMgmt::dump_status();
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
