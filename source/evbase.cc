/*
 * evbase.cc
 *
 *  Created on: 04.12.2016
 *      Author: ed
 */

#include "evbase.h"
#include "fileio.h"

#warning test legacy pipe

namespace acng
{

bool tEventBase::InitEvents(int fd, event_callback_fn cb)
{
	if(!m_eventRecv)
		m_eventRecv = event_new(g_ebase, fd, EV_READ, cb_conn, this);
	if(!m_eventSendRecv)
		m_eventSendRecv = event_new(g_ebase, fd, EV_READ|EV_WRITE, cb_conn, this);
	if(!m_eventRecv || !m_eventSendRecv)
		return false;
	return ! event_add(m_eventRecv, &GetNetworkTimeout());

}

void tEventBase::DeleteEvents()
{
	if(m_eventRecv)
		event_free(m_eventRecv);
	if(m_eventSendRecv)
		event_free(m_eventSendRecv);
	m_eventRecv = m_eventSendRecv = nullptr;
}

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

} // namespace acng
