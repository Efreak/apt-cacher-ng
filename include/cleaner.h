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
#include <array>

namespace acng
{

/**
 * @brief Primitive task scheduler for internal helper functions
 *
 * It's based on few assumptions: a) called methods are static,
 * b) called method return the next time they want to be called
 * c) called method can be run at any time without side-effects
 * (they have to check the need of their internal work by themselves)
 *
 * The implementation comes from a trade-off. Data size is not big enough to justify use of priority_queue,
 * and it also makes not much sense since time points need to be adjusted for specific usecases without
 * need to invalidate previous entries sitting in the prio-queue.
 *
 */
class ACNG_API cleaner : public base_with_condition
{
public:
	void Init();
	virtual ~cleaner();

	void WorkLoop();
	void Stop();

	enum eType : char
	{
		TYPE_EXFILEITEM, TYPE_ACFGHOOKS, /* TYPE_EXDNS,*/ TYPE_EXCONNS,
		DNS_CACHE,
		ETYPE_MAX
	};
	void ScheduleFor(time_t when, eType what);
	void dump_status();
	static cleaner& GetInstance(bool initAsNoop=false);

private:
	cleaner(bool noop=false);
	pthread_t m_thr;
	std::array<time_t,cleaner::ETYPE_MAX> stamps;
	bool m_terminating = false;
	bool m_noop=false;
};

}

#endif /* CLEANER_H_ */
