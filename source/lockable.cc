
#include "debug.h"

#include <config.h>
#include "lockable.h"

#ifdef HAVE_LINUX_EVENTFD
#include <sys/eventfd.h>
#endif
#include "fileio.h"

#include <chrono>

namespace acng
{
bool base_with_condition::wait_until(lockuniq& uli, time_t nUTCsecs, long msec)
{
	auto tpUTC = std::chrono::system_clock::from_time_t(nUTCsecs);
	tpUTC += std::chrono::milliseconds(msec);
	auto r = m_obj_cond.wait_until(uli._guard, tpUTC);
	return std::cv_status::timeout == r;
}

bool base_with_condition::wait_for(lockuniq& uli, long secs, long msec)
{
	auto r = m_obj_cond.wait_for(uli._guard, std::chrono::milliseconds(msec + secs*1000));
	return std::cv_status::timeout == r;
}

#warning test legacy pipe

tEventFd::~tEventFd()
{

#ifdef HAVE_LINUX_EVENTFD
	forceclose(m_wakeventfd);
#else
	forceclose(m_wakepipe[0]);
	forceclose(m_wakepipe[1]);
#endif
}

bool tEventFd::setupEventFd()
{
#ifdef HAVE_LINUX_EVENTFD
	if (m_wakeventfd != -1)
		return true;

	m_wakeventfd = eventfd(0, 0);

	if (m_wakeventfd == -1)
		return false;

	set_nb(m_wakeventfd);

#else
	if(m_wakepipe[0] != -1)
	return true;

	if (0 == pipe(m_wakepipe))
	{
		set_nb(m_wakepipe[0]);
		set_nb(m_wakepipe[1]);
	}
	else
	{
		m_wakepipe[0] = -1;
		return false;
	}

#endif

	return true;
}

void tEventFd::eventFetch()
{
#ifdef HAVE_LINUX_EVENTFD
					eventfd_t xtmp;
					int tmp;
					do
					{
						tmp = eventfd_read(getEventReadFd(), &xtmp);
					} while (tmp < 0 && (errno == EINTR || errno == EAGAIN));

#else
					for (int tmp; read(getEventReadFd(), &tmp, 1) > 0;)
					;
#endif

}

void tEventFd::eventPoke()
{
#ifdef HAVE_LINUX_EVENTFD
       while(eventfd_write(getEventWriteFd(), 1)<0);
#warning fixme-poke
#else
       POKE(getEventWriteFd());
#endif
}



}
