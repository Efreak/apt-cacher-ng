#ifndef _LOCKABLE_H
#define _LOCKABLE_H

#include <mutex>
#include <condition_variable>
#include <atomic>
#ifdef HAVE_SHARED_MUTEX
#include <shared_mutex>
#endif

namespace acng
{

typedef std::mutex acmutex;
typedef std::lock_guard<std::mutex> lguard;
typedef std::unique_lock<std::mutex> ulock;

struct base_with_mutex
{
	acmutex m_obj_mutex;
};

// little adapter for more convenient use
struct lockguard {
	std::lock_guard<acmutex> _guard;
	lockguard(acmutex& mx) : _guard(mx) {}
	lockguard(acmutex* mx) : _guard(*mx) {}
	lockguard(base_with_mutex& mbase) : _guard(mbase.m_obj_mutex) {}
	lockguard(base_with_mutex* mbase) : _guard(mbase->m_obj_mutex) {}
	lockguard(std::shared_ptr<base_with_mutex> mbase) : _guard(mbase->m_obj_mutex) {}
};

struct lockuniq {
	std::unique_lock<acmutex> _guard;
	lockuniq(acmutex& mx) : _guard(mx) {}
	lockuniq(base_with_mutex& mbase) : _guard(mbase.m_obj_mutex) {}
	lockuniq(base_with_mutex* mbase) : _guard(mbase->m_obj_mutex) {}
	lockuniq(std::shared_ptr<base_with_mutex> mbase) : _guard(mbase->m_obj_mutex) {}
	void unLock() { _guard.unlock();}
	void reLock() { _guard.lock(); }
	void reLockSafe() { if(!_guard.owns_lock()) _guard.lock(); }
};

struct base_with_condition : public base_with_mutex
{
	std::condition_variable m_obj_cond;
	void notifyAll() { m_obj_cond.notify_all(); }
	void wait(lockuniq& uli) { m_obj_cond.wait(uli._guard); }
	bool wait_until(lockuniq& uli, time_t nUTCsecs, long msec);
	bool wait_for(lockuniq& uli, long secs, long msec);
};

#define setLockGuard std::lock_guard<acmutex> local_helper_lockguard(m_obj_mutex);

struct atomic_spinlock
{
	typedef std::atomic_bool flagvariable;
	flagvariable & m_flagVar;
	atomic_spinlock(flagvariable &flagVar) : m_flagVar(flagVar)
	{
		for(;;)
		{
			bool exp=false;
			if(m_flagVar.compare_exchange_weak(exp, true))
				return;
		}
	}
	~atomic_spinlock()
	{
		m_flagVar.store(false);
	}
};

#ifdef HAVE_SHARED_MUTEX

using std::shared_mutex;
typedef std::shared_lock<std::shared_mutex> reader_lock;

#else

typedef std::mutex shared_mutex;
typedef std::lock<std::mutex> reader_lock;

#endif

// helper to start and close a semi-critical section (where writes can happen)
struct seqlock_writesection
{
	std::atomic_int& m_var;
	seqlock_writesection(std::atomic_int& theVar) : m_var(theVar)	{ m_var.fetch_add(1); }
	~seqlock_writesection() { m_var.fetch_add(1); }
};

/**
 * Repeat execution of readfunc until a consistent read was guaranteed.
 */
template<typename TFUNC>
void seqlock_readsection(std::atomic_int& flagVar, TFUNC &readfunc)
{
	int refVal;
	do
	{
		refval = flagVar;
		readfunc();
	} while ((refVal & 2) || (refVal != flagVar));
}

}

#endif
