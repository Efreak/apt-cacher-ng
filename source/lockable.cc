
#define LOCAL_DEBUG
#include "debug.h"

#include <config.h>
#include "lockable.h"
#include <inttypes.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>

bool lockable::tryLock() {
	return EBUSY!=pthread_mutex_trylock(&m_obj_mutex);
}

bool condition::wait_until(time_t nUTCsecs, long msec)
{
	if(nUTCsecs < 0 || msec < 0)
		return false; // looks suspicious, report timeout, immediately
	if(msec>=1000)
	{
		nUTCsecs += (msec/1000);
		msec %=1000;
	}
	struct timespec timeout = {nUTCsecs, msec*1000};
	auto r = pthread_cond_timedwait(&m_obj_cond, &m_obj_mutex, &timeout);

#ifdef _DARWIN_C_SOURCE
    // From: Andrew Sharpe <andrew.sharpe79@gmail.com>
    // Date: Sat, 29 Dec 2012 19:21:11 +1000
	// ...
    // kludge to work around what looks like wrapping issues -
	// I'd like to know the implementation of pthread_cond_timedwait
	// to get a definitive solution to this, as tracing through the
	// headers on OSX lead me to believe that timespec.tv_sec is of
	// type __darwin_time_t and that can handle values up to
	// 9223372036854775807 (on this machine), however this
	// implementation breaks with any value greater than
	// 2147483647 (std::numeric_limits<int>::max())

	if (r==ETIMEDOUT && nUTCsecs == END_OF_TIME)
	{
		timeout.tv_sec = MAX_VAL(int);
		r=pthread_cond_timedwait(&m_obj_cond, &m_obj_mutex, &timeout);
	}
#endif

	return r==ETIMEDOUT;
}
