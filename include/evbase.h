/*
 * evbase.h
 *
 *  Created on: 04.12.2016
 */

#ifndef INCLUDE_EVBASE_H_
#define INCLUDE_EVBASE_H_

#include "config.h"
#include <event.h>
namespace acng
{

class tEventBase
{
public:
	bool InitEvents(int fd, event_callback_fn cb) noexcept;
	void DeleteEvents();
protected:
	  struct event *m_eventRecv = 0, *m_eventSendRecv = 0;
};

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


#endif /* INCLUDE_EVBASE_H_ */
