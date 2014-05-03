/*
 * cleaner.h
 *
 *  Created on: 05.02.2011
 *      Author: ed
 */

#ifndef CLEANER_H_
#define CLEANER_H_

#include "lockable.h"
#include <ctime>
#include <limits>

/**
 * @brief Primitive task scheduler for internal helper functions
 *
 * It's based on few assumptions: a) called methods are static,
 * b) called method return the next time they want to be called
 * c) called method can be run at any time without side-effects
 * (they have to check the need of their internal work by themselves)
 */
class cleaner : public condition
{
public:
	cleaner();
	void Init();
	virtual ~cleaner();

	void WorkLoop();
	void Stop();

	enum eType
	{
		TYPE_EXFILEITEM, TYPE_ACFGHOOKS, TYPE_EXDNS, TYPE_EXCONNS,
		TYPE_STOPSCHED, ETYPE_MAX
	};
	void ScheduleFor(time_t when, eType what);
	void dump_status();

private:
	pthread_t m_thr;
	time_t stamps[cleaner::ETYPE_MAX];
};

extern cleaner g_victor; // ... down to the nap

#endif /* CLEANER_H_ */
