#ifndef _LOCKABLE_H
#define _LOCKABLE_H

#include <pthread.h>

/*
 * This is a custom implementation of synchronization objects for flexible use in different
 * parts of ACNG, including RAII style helper objects.
 *
 * The implementation is somewhat similar to what's available in Boost and C++11 now
 * but only because it was driven by the same ideas and needs, and it was developed long
 * before C++11 and explicitly avoiding the requirement of a Boost installation.
 */

class lockguard;

#ifdef DEBUG_never
#include <iostream>
#define __lock_yell(x) std::cerr <<"### " x "\n";
#else
#define __lock_yell(x)
#endif

class lockable
{
public:
	inline lockable()
	{
		pthread_mutex_init(&m_obj_mutex, NULL);
	}
	virtual ~lockable()
	{
		pthread_mutex_destroy(&m_obj_mutex);
	}
	inline void lock()
	{
		pthread_mutex_lock(&m_obj_mutex);
	}
	bool tryLock();
	inline void unlock()
	{
		pthread_mutex_unlock(&m_obj_mutex);
	}

protected:
	friend class lockguard;
	pthread_mutex_t m_obj_mutex;
};

class lockguard
{
public:

	// lock() helper sets the bool flag, no need to pre-initialize here
	inline lockguard(pthread_mutex_t *m, bool bInitiallyLock = true) :
			l(*m), bLocked(false)
	{
		if (bInitiallyLock)
			lock();
	}
	inline lockguard(pthread_mutex_t &m, bool bInitiallyLock = true) :
			l(m), bLocked(false)
	{
		if (bInitiallyLock)
			lock();
	}
	inline lockguard(lockable & cl, bool bInitiallyLock = true) :
			l(cl.m_obj_mutex), bLocked(false)
	{
		if (bInitiallyLock)
			lock();
	}

	inline lockguard(lockable *cl, bool bInitiallyLock = true) :
			l(cl->m_obj_mutex), bLocked(false)
	{
		if (bInitiallyLock)
			lock();
	}
	inline void unLock()
	{
		if (bLocked)
		{
			pthread_mutex_unlock(&l);
			bLocked = false;
			__lock_yell ("LOCK RELEASED");
		}
	}
	inline void reLock()
	{
		if (!bLocked)
			lock();
	}
	~lockguard()
	{
		unLock();
	}

private:
	pthread_mutex_t& l;
	bool bLocked;

	inline void lock()
	{
		pthread_mutex_lock(&l);
		bLocked = true;
		__lock_yell ("LOCK SET");
	}

	// not to be copied ever
	lockguard(const lockguard&);
	lockguard& operator=(const lockguard&);
};

//#define setLockGuard ldbgvl(DBGSPAM, "Locking @" << __LINE__); lockguard __lockguard(&__mutex);
#define setLockGuard lockguard local_helper_lockguard(&m_obj_mutex);

// more sophisticated condition class, includes the required mutex
class condition: public lockable
{

protected:
	pthread_cond_t m_obj_cond;

public:
	inline condition() :
			lockable()
	{
		pthread_cond_init(&m_obj_cond, NULL);
	}
	~condition()
	{
		pthread_cond_destroy(&m_obj_cond);
	}
	inline void wait()
	{
		pthread_cond_wait(&m_obj_cond, &m_obj_mutex);
	}

#if 0
	/** \brief Waits for the specified period of time or a signal or external notification
	 * @return true if awaken by another thread or signal, false if run into timeout.
	 * @param secs Seconds to wait
	 * @param msecs Extra milliseconds to wait, optional value
	 */
	bool wait(time_t secs, long msecs=0);
#endif

	/** \brief Waits until the specified moment is reached or a signal
	 * or external notification happened
	 *
	 * @return true waited to the end, false when awaken prematurely
	 * @param secs Seconds to wait
	 * @param msecs Extra milliseconds to wait, optional value
	 */
	bool wait_until(time_t nUTCsecs, long msec);
	inline void notifyAll()
	{
		pthread_cond_broadcast(&m_obj_cond);
	}
	void notify()
	{
		pthread_cond_signal(&m_obj_cond);
	}
};

#endif
