
#define LOCAL_DEBUG
#include "debug.h"

#include <config.h>
#include "lockable.h"
#include <inttypes.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>

//#include <iostream>
//using namespace MYSTD;

lockable::lockable() { pthread_mutex_init(&__mutex, NULL); /*MYSTD::cout << "inited lock:" <<&__mutex<<endl;*/}
lockable::~lockable() { pthread_mutex_destroy(&__mutex); }
void lockable::lock() { /*cout << "locking: " << &__mutex<<endl;*/ pthread_mutex_lock(&__mutex); }
void lockable::unlock() { /*cout << "releasing: " << &__mutex<<endl;*/ pthread_mutex_unlock(&__mutex); }
bool lockable::tryLock() { return EBUSY!=pthread_mutex_trylock(&__mutex); }

condition::condition() : lockable() { pthread_cond_init(&__cond, NULL); }
condition::~condition() { pthread_cond_destroy(&__cond); }
void condition::wait() { 
    //ldbg("waiting on " << (uint64_t) &__cond); 
    pthread_cond_wait(&__cond, &__mutex); 
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
	return ETIMEDOUT == pthread_cond_timedwait(&__cond, &__mutex, &timeout);
}

#if 0
bool condition::wait(time_t sec, long msec)
{
	if(sec<0 || msec<0)
		return false; // looks suspicious, report timeout, immediately

    struct timeval now;
    struct timespec timeout;
    gettimeofday(&now, NULL);

    timeout.tv_sec = now.tv_sec + sec;
    timeout.tv_nsec = now.tv_usec * 1000 + msec;
    if(timeout.tv_nsec>=1000000)
    {
    	timeout.tv_sec += (timeout.tv_nsec / 1000000);
    	timeout.tv_nsec %= 1000000;
	}

    // make sure to not cause harm if it ever underflows
    if(timeout.tv_sec < 0)
    	timeout.tv_sec = MAX_VAL(time_t);

#ifdef _DARWIN_C_SOURCE
    // From: Andrew Sharpe <andrew.sharpe79@gmail.com>
    // Date: Sat, 29 Dec 2012 19:21:11 +1000
    // (slightly modified)

    // kludge to work around what looks like wrapping issues -
	// I'd like to know the implementation of pthread_cond_timedwait
	// to get a definitive solution to this, as tracing through the
	// headers on OSX lead me to believe that timespec.tv_sec is of
	// type __darwin_time_t and that can handle values up to
	// 9223372036854775807 (on this machine), however this
	// implementation breaks with any value greater than
	// 2147483647 (std::numeric_limits<int>::max())
	if (timeout.tv_sec > MAX_VAL(int))
		timeout.tv_sec = MAX_VAL(int);
#endif

	return ETIMEDOUT != pthread_cond_timedwait(&__cond, &__mutex, &timeout);
}
#endif

void condition::notifyAll() { 
    //ldbg("broadcast " << (uint64_t) &__cond);
    pthread_cond_broadcast(&__cond);
}

void condition::notify() { 
    //ldbg("signal " << (uint64_t) &__cond);
    pthread_cond_signal(&__cond); 
}



//void condition::notifyWithLocking() { lock(); notify(); unlock();}
