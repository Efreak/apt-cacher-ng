
#include "meta.h"
#include "sockio.h"
#include "debug.h"
#include <unordered_map>

namespace acng
{
using namespace std;

// those data structures are used by main thread only
// helper structure with metadata which can be passed around
unordered_map<int,time_t> g_discoTimeouts;
char crapbuf[40];

void termsocket_now(int fd, void *p = nullptr)
{
	::shutdown(fd, SHUT_RD);
	forceclose(fd);
	g_discoTimeouts.erase(fd);
	if(p) event_free((event*)p);
}

void linger_read(int fd, short what, void *p)
{
	if ((what & EV_TIMEOUT) || !(what & EV_READ))
		return termsocket_now(fd, p);
	// ok, have to read junk or terminating zero-read
	while (true)
	{
		int r = recv(fd, crapbuf, sizeof(crapbuf), MSG_WAITALL);
		if (0 == r)
			return termsocket_now(fd, p);
		if (r < 0)
		{
			if (errno == EAGAIN)
			{
				// come again later
				CTimeVal exp;
				event_add((event*) p, exp.Remaining(g_discoTimeouts[fd]));
				return;
			}
			if (errno == EINTR)
				continue;
			// some other error? Kill it
			return termsocket_now(fd, p);
		}
	}
}

/*! \brief Helper to flush data stream contents reliable and close the connection then
 * DUDES, who write TCP implementations... why can this just not be done easy and reliable? Why do we need hacks like the method below?
 For details, see: http://blog.netherlabs.nl/articles/2009/01/18/the-ultimate-so_linger-page-or-why-is-my-tcp-not-reliable
 * Using SO_LINGER is also dangerous, see https://www.nybek.com/blog/2015/04/29/so_linger-on-non-blocking-sockets
 *
 */
void termsocket_async(int fd, event_base* base)
{
	event* ev(nullptr);
	try
	{
		LOGSTART2s("::termsocket", fd);
		if (!fd)
			return;
		// initiate shutdown, i.e. sending FIN and giving the remote some time to confirm
		::shutdown(fd, SHUT_WR);
		int r = read(fd, crapbuf, sizeof(crapbuf));
		if (r == 0) // fine, we are done
			return termsocket_now(fd, nullptr);
		LOG("waiting for peer to react");
		auto ev = event_new(base, fd, EV_READ, linger_read,
				event_self_cbarg());
		g_discoTimeouts[fd] = GetTime() + cfg::discotimeout;
		struct timeval tmout { cfg::discotimeout, 42 };
		if (ev && 0 == event_add(ev, &tmout))
			return; // will cleanup in the callbacks
	} catch (...)
	{
	}
	// error cleanup... EOM?
	if (ev)
		event_free(ev);
	justforceclose(fd);
}

void set_connect_sock_flags(evutil_socket_t fd)
{
		evutil_make_socket_nonblocking(fd);
#ifndef NO_TCP_TUNNING
		int yes(1);
		::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
		::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
}

}
