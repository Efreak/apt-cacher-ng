#ifndef _LOCKABLE_H
#define _LOCKABLE_H

#include <pthread.h>

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
	lockable();
	virtual ~lockable();
	void lock();
	bool tryLock();
	void unlock();
	//pthread_mutex_t * MutexRef() { return &__mutex; }

protected:
	friend class lockguard;
	pthread_mutex_t __mutex;
};

class lockguard
{
public:

	// lock() helper sets the bool flag, no need to pre-initialize here
	inline lockguard(pthread_mutex_t *m) :
		l(m), bLocked(false)
	{
		lock();
	}
	inline lockguard(pthread_mutex_t & m) :
		l(&m), bLocked(false)
	{
		lock();
	}
	inline lockguard(lockable & cl) :
		l(&cl.__mutex), bLocked(false)
	{
		lock();
	}

	inline lockguard(lockable *cl, bool bInitiallyLock=true) :
		l(&cl->__mutex), bLocked(false)
	{
		if (bInitiallyLock)
			lock();
	}
	inline void unLock()
	{
		if (bLocked)
		{
			pthread_mutex_unlock(l);
			bLocked=false;
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
	pthread_mutex_t *l;
	bool bLocked;

	inline void lock()
	{
		pthread_mutex_lock(l);
		bLocked=true;
		__lock_yell ("LOCK SET");
	}

	// not to be copied ever
	lockguard(const lockguard&);
	lockguard& operator=(const lockguard&);
};

//#define setLockGuard ldbgvl(DBGSPAM, "Locking @" << __LINE__); lockguard __lockguard(&__mutex);
#define setLockGuard lockguard __lockguard(&__mutex);

// more sophisticated condition class, includes the required mutex
class condition : public lockable {
    public:
        condition();
        ~condition();
        void wait();

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
        void notifyAll();
        void notify();

    protected:
        pthread_cond_t __cond;
};

/*
// modified version of lockguard, broadcasting potential sleepers

class notifyguard {
    public:
        notifyguard(pthread_mutex_t *m, pthread_cond_t *d):l(m),c(d){pthread_mutex_lock(m);}
        ~notifyguard() {pthread_cond_broadcast(c); pthread_mutex_unlock(l);}
    private:
        pthread_mutex_t *l;
        pthread_cond_t *c;
};

#define setNotifyGuard notifyguard __notifyguard(&__mutex, &__cond);
*/

#if 0 // generalized version of the thread pool which might be used to conns and dlconns
// however, the requirements are slightly different so most likely it would suck

class tRunnable
{
public:
	virtual void WorkLoop() =0;
	virtual ~tRunable() {};
};

class tThreadPool
{
public:
	tThreadPool(size_t nThreadMax, size_t nMaxStandby, size_t nMaxBacklog);
	void AddRunnable(tRunnable *);
	void ResetBacklog();
	bool StartThreadsAsNeeded();
	void StopAll();
private:
	size_t m_nThreadMax, m_nMaxStandby, m_nMaxBacklog;
	std::vector<pthread_t> m_threads;
	condition m_syncCond;
};
#endif

#endif
