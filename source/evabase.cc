#include "evabase.h"

#include "meta.h"
#include "debug.h"

#include <event2/dns.h>
#include <event2/util.h>

#ifdef HAVE_SD_NOTIFY
#include <systemd/sd-daemon.h>
#endif

using namespace std;

namespace acng
{

namespace conserver
{
// forward declarations for the pointer checks
//void cb_resume(evutil_socket_t fd, short what, void* arg);
void do_accept(evutil_socket_t server_fd, short what, void* arg);
}
// that's from caddrinfo.cc
void cb_invoke_dns_res(int result, short what, void *arg);


event_base* evabase::base;
evdns_base* evabase::dnsbase;

struct t_event_desctor {
	evutil_socket_t fd;
	event_callback_fn callback;
	void *arg;
};

/**
 * Forcibly run each callback and signal shutdown.
 */
int teardown_event_activity(const event_base*, const event* ev, void* ret)
{
	t_event_desctor r;
	event_base *nix;
	short what;
	auto lret((deque<t_event_desctor>*)ret);
	event_get_assignment(ev, &nix, &r.fd, &what, &r.callback, &r.arg);
#ifdef DEBUG
	if(r.callback == conserver::do_accept)
		cout << "stop accept: " << r.arg << endl;
	if(r.callback == cb_invoke_dns_res)
		cout << "stop dns req: " << r.arg <<endl;
#endif
	if(r.callback == conserver::do_accept || r.callback == cb_invoke_dns_res)
		lret->emplace_back(move(r));
	return 0;
}

ACNG_API int evabase::MainLoop()
{
		LOGSTART2s("Run", "GoGoGo");

	#ifdef HAVE_SD_NOTIFY
		sd_notify(0, "READY=1");
	#endif

		int r=event_base_loop(evabase::base, EVLOOP_NO_EXIT_ON_EMPTY);

		g_global_shutdown = true;

		if(evabase::dnsbase)
			evdns_base_free(evabase::dnsbase, 1), dnsbase = nullptr;

		// send teardown hint to all event callbacks
		deque<t_event_desctor> todo;
		event_base_foreach_event(evabase::base, teardown_event_activity, &todo);
		for (const auto &ptr : todo)
		{
			DBGQLOG("Notifying event on " << ptr.fd);
			ptr.callback(ptr.fd, TEARDOWN_HINT, ptr.arg);
		}

#ifdef HAVE_SD_NOTIFY
	sd_notify(0, "READY=0");
#endif
	return r;
}

evabase::evabase()
{
	evabase::base = event_base_new();
	evabase::dnsbase = evdns_base_new(evabase::base, 1);
}

evabase::~evabase()
{
	if(evabase::base)
		event_base_free(evabase::base);
}


}
