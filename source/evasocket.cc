/*
 * evasocket.cc
 *
 *  Created on: 15.03.2019
 *      Author: EB
 */

#include "evasocket.h"
#include "evabase.h"

#include "fileio.h"
#include <event2/event.h>
#include <memory>

acng::evasocket::evasocket(int fd) : m_fd(fd)
{
}

acng::evasocket::~evasocket()
{
	checkforceclose(m_fd);
}

std::shared_ptr<acng::evasocket> acng::evasocket::create(int fd)
{
	return std::shared_ptr<acng::evasocket>(new evasocket(fd));
}

acng::event_socket::event_socket(std::shared_ptr<evabase> ev_base, std::shared_ptr<evasocket> sock,
		short create_flags,
		std::function<void(const std::shared_ptr<evasocket> & sock, short)> action)
:	m_evbase(ev_base), m_sock(sock), m_parent_action(action)
{
	m_event = event_new(ev_base->base, sock->fd(), create_flags, event_socket::on_io, this);
}

acng::event_socket::~event_socket()
{
	if(m_event)
		event_free(m_event);
}

void acng::event_socket::on_io(evutil_socket_t, short what, void* uptr)
{
	auto es = (event_socket*) uptr;
	es->m_parent_action(es->m_sock, what);
}

void acng::event_socket::enable(const struct timeval *tv)
{
	if(m_event)
		event_add(m_event, tv);
}

void acng::event_socket::disable()
{
	if(m_event)
		event_del(m_event);
}
/*
acng::event_socket::event_socket(event_socket&& o)
{
	m_evbase.swap(o.m_evbase);
	m_sock.swap(o.m_sock);
	m_parent_action.swap(o.m_parent_action);
	m_event = o.m_event;
	o.m_event = nullptr;
}
*/

acng::socket_activity_base::socket_activity_base(std::shared_ptr<evabase> evbase)
: m_evbase(evbase)
{
	m_evbase->register_activity(this);
}

acng::socket_activity_base::~socket_activity_base()
{
	m_evbase->unregister_activity(this);
}
/*
std::shared_ptr<acng::socket_activity_base> acng::socket_activity_base::self_lock()
{
	m_selfref = std::shared_from_this(this);
	m_evbase->register_activity(m_selfref);
	return m_selfref;
}

std::shared_ptr<acng::socket_activity_base> acng::socket_activity_base::release_self_lock()
{
	decltype(m_selfref) ret;
	if(m_selfref)
	{
		m_evbase->unregister_activity(m_selfref);
		ret.swap(m_selfref);
	}
	return ret;
}
*/
