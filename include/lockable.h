#ifndef _LOCKABLE_H
#define _LOCKABLE_H

#include <mutex>
#include <condition_variable>

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


class tEventFd
{
	#ifdef HAVE_LINUX_EVENTFD
	int m_wakeventfd = -1;
#else
      int m_wakepipe[2] = {-1, -1};
#endif

public:
      ~tEventFd();
      bool setupEventFd();
#ifdef HAVE_LINUX_EVENTFD
      int getEventWriteFd() { return m_wakeventfd; }
      int getEventReadFd() { return m_wakeventfd; }
#else
      int getEventWriteFd() { return m_wakepipe[1]; }
      int getEventReadFd() { return m_wakepipe[0]; }
#endif
      void eventFetch();
      void eventPoke();
};

}

#endif
