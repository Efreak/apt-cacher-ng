#ifndef __evasocket_h__
#define __evasocket_h__

#include <event2/util.h>
#include <memory>

#include "meta.h"

struct event;

namespace acng
{

class evabase;

/**
 * Event lib abstraction - destructible socket wrapper
 *
 * Auto-closer for a socket descriptor
 */
class evasocket
{
	evutil_socket_t m_fd;
	evasocket(evutil_socket_t fd);
public:
	~evasocket();
	evutil_socket_t fd() { return m_fd; }
	static std::shared_ptr<evasocket> create(evutil_socket_t fd);
};

/**
 * Reference-counting-friendly helper which binds an event to a socket.
 */
class event_socket
{
	struct event * m_event = nullptr;
	std::shared_ptr<evabase> m_evbase;
	std::shared_ptr<evasocket> m_sock;
	std::function<void(const std::shared_ptr<evasocket>& sock, short)> m_parent_action;

public:
	event_socket(std::shared_ptr<evabase> ev_base,
			std::shared_ptr<evasocket> sock,
			short create_flags,
			std::function<void(const std::shared_ptr<evasocket>& sock, short)> action);
	~event_socket();

	// activate the event once (does not make sense with EV_PERSIST)
	void enable();
	// disable a previously enabled event (does not make much sense with EV_PERSIST unless suppressing one known pending execution is needed)
	void disable();

	// interface to libevent callbacks
	static void on_io(evutil_socket_t, short what, void *uptr);

	// moving is ok, copying: not!
	event_socket(event_socket&&);
private:
	event_socket(const event_socket&) = delete;
};

/**
 * A free running activity tracker for interaction with a single socket.
 * Will notify the parent about its destruction.
 */
class socket_activity_base
{
	// keep our base alive
	std::shared_ptr<evabase> m_evbase;
public:
	socket_activity_base(std::shared_ptr<evabase> m_evbase);
	// release the back reference from the main base
	~socket_activity_base();
};

}

#endif
