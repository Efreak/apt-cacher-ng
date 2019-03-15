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

#if 0
acng::evasockevent::evasockevent(std::shared_ptr<evabase> ev_base, int fd)
: m_fd(fd), m_evbase(ev_base)
{
	auto mask = 0;
	m_event = event_new(ev_base->base, fd,
	    mask, event_callback_fn cb,
	    void *arg);
}

acng::evasockevent::~evasockevent()
{
	if(m_event)
		event_free(m_event);
}
#endif

acng::evasocket::evasocket(int fd) : m_fd(fd)
{
}

acng::evasocket::~evasocket()
{
	if(m_fd != -1) forceclose(m_fd);
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

void acng::event_socket::enable()
{
	if(m_event)
		event_add(m_event, nullptr);
}

void acng::event_socket::disable()
{
	if(m_event)
		event_del(m_event);
}

acng::event_socket::event_socket(event_socket&& o)
{
	m_evbase.swap(o.m_evbase);
	m_sock.swap(o.m_sock);
	m_parent_action.swap(o.m_parent_action);
	std::swap(m_event, o.m_event);
}
