#include "acngbase.h"
#include "meta.h"
#include "debug.h"
#include "lockable.h"

#include <thread>
#include <future>
#include <event2/util.h>
#include <event2/thread.h>

#ifdef HAVE_SD_NOTIFY
#include <systemd/sd-daemon.h>
#endif

using namespace std;

//XXX: add an extra task once per hour or so, optimizing all caches

namespace acng
{
//std::atomic<bool> evabase::in_shutdown = ATOMIC_VAR_INIT(false);


#if false

namespace conserver
{
// forward declarations for the pointer checks
//void cb_resume(evutil_socket_t fd, short what, void* arg);
void do_accept(evutil_socket_t server_fd, short what, void* arg);
}

struct t_event_desctor {
	const event *ev;
	evutil_socket_t fd;
	event_callback_fn callback;
	void *arg;
};

void cb_clean_cached_con(evutil_socket_t server_fd, short what, void* arg);

/**
 * Get interesting callbacks which need to be triggered
 */
int cb_collect_event(const event_base*, const event* ev, void* ret)
{
	t_event_desctor r;
	event_base *nix;
	short what;
	auto lret((deque<t_event_desctor>*)ret);
	r.ev = ev;
	event_get_assignment(ev, &nix, &r.fd, &what, &r.callback, &r.arg);
#ifdef DEBUG
	if(r.callback == conserver::do_accept)
		cout << "stop accept: " << r.arg << endl;
#endif
	if(r.callback == conserver::do_accept)
		lret->emplace_back(move(r));
	if(r.callback == acng::cb_clean_cached_con)
		lret->emplace_back(move(r));
	return 0;
}
#endif

class tEventActivity : public IActivity
{
public:
	event_base *m_base = nullptr;
	std::thread::id mainThreadId;


	struct event *handover_wakeup;
	const struct timeval timeout_asap{0,0};
	deque<tCancelableAction> incoming_q, processing_q;
	mutex handover_mx;

	static void cb_handover(evutil_socket_t sock, short what, void* arg)
	{
		auto me((tEventActivity*)arg);
		{
			lockguard g(me->handover_mx);
			me->processing_q.swap(me->incoming_q);
		}
		for(const auto& ac: me->processing_q)
			ac(false);
		me->processing_q.clear();
	}


	tEventActivity()
	{
		evthread_use_pthreads();
		m_base = event_base_new();
		handover_wakeup =  evtimer_new(m_base, cb_handover, this);
	}



	/**
	 *
	 * Runs the main loop for a program around the event_base loop.
	 * When finished, clean up some resources left behind (fire off specific events
	 * which have actions that would cause blocking otherwise).
	 */
	int MainLoop()
	{
		LOGSTARTFUNC;
		#ifdef HAVE_SD_NOTIFY
			sd_notify(0, "READY=1");
		#endif
			mainThreadId = this_thread::get_id();

			int r=event_base_loop(m_base, EVLOOP_NO_EXIT_ON_EMPTY);
			event_base_loop(m_base, EVLOOP_NONBLOCK);

#if false
			// send teardown hint to all event callbacks
			deque<t_event_desctor> todo;
			event_base_foreach_event(evabase::base, cb_collect_event, &todo);
			for (const auto &ptr : todo)
			{
				if(!event_pending(ptr.ev, EV_READ|EV_WRITE|EV_SIGNAL|EV_TIMEOUT, nullptr)) continue;
				DBGQLOG("Notifying event on " << ptr.fd);
				ptr.callback(ptr.fd, EV_TIMEOUT, ptr.arg);
			}
			event_base_loop(base, EVLOOP_NONBLOCK);
#endif
	#ifdef HAVE_SD_NOTIFY
		sd_notify(0, "READY=0");
	#endif
		return r;
	}

	/**
	 * Tell the executing loop to cancel unfinished activities and stop.
	 */
	void SignalShutdown()
	{
		if(m_base)
			event_base_loopbreak(m_base);
	}

	/**
	 * Push an action into processing queue. In case operation is not possible, runs the action with the cancel flag (bool argument set to true)
	 */
	void Post(tCancelableAction&& act) override
	{
		{
			lockguard g(handover_mx);
			incoming_q.emplace_back(move(act));
		}
		event_add(handover_wakeup, &timeout_asap);
	}
	/*
	 * Like Post, but if the thread is the same as main thread, run the action in-place
	 */
	void PostOrRun(tCancelableAction&& act) override
	{
		if(this_thread::get_id() == mainThreadId)
			act(false);
		else
			Post(move(act));
	}

	std::thread::id GetThreadId() override
	{
		return mainThreadId;
	}
};


class tThreadedActivity : public IActivity, public base_with_condition
{
public:
	std::thread m_thread;
	std::thread::id mainThreadId;
	deque<tCancelableAction> incoming_q, processing_q;

	tThreadedActivity()
	{
	}

	// start a new background thread, make sure that it became operational when exiting
	IActivity* Spawn(tSysRes& rc)
	{
		std::promise<bool> confirm_start;
		std::thread([&]()
				{
			mainThreadId = this_thread::get_id();
			confirm_start.set_value(true);

			lockuniq q(this);

			while (!rc.in_shutdown)
			{
				if (incoming_q.empty())
				wait(q);
				processing_q.swap(incoming_q);
				for (const auto &ac : processing_q)
				{
					q.unLock();
					ac(rc.in_shutdown);
					q.reLock();
				}
				processing_q.clear();
			}
			q.unLock();
			if(rc.in_shutdown && !incoming_q.empty())
			{
				for (const auto &ac : processing_q) ac(true);
			}

		}).swap(m_thread);

		return confirm_start.get_future().get() ? this : nullptr;
	}

	/**
	 * Push an action into processing queue. In case operation is not possible, runs the action with the cancel flag (bool argument set to true)
	 */
	void Post(tCancelableAction &&act) override
	{
		lockuniq q(this);
		incoming_q.emplace_back(move(act));
		notifyAll();
	}
	/*
	 * Like Post, but if the thread is the same as main thread, run the action in-place
	 */
	void PostOrRun(tCancelableAction&& act) override
	{
		if(this_thread::get_id() == mainThreadId)
			act(false);
		else
			Post(move(act));
	}

	std::thread::id GetThreadId() override
	{
		return mainThreadId;
	}

	~tThreadedActivity()
	{
		if(m_thread.joinable())
			m_thread.join();
	}
};

void AddDefaultDnsBase(tSysRes &src);

std::unique_ptr<tSysRes> CreateRegularSystemResources()
{
	struct tSysResReal : public tSysRes
	{
		event_base * GetEventBase() override
		{
			return static_cast<tEventActivity*>(this->fore)->m_base;
		}
		~tSysResReal()
		{
			delete back;
			delete meta;
			delete fore;
		}
		int MainLoop() override { return static_cast<tEventActivity*>(fore)->MainLoop(); };
		void SignalShutdown() override {
			in_shutdown.store(true);
			static_cast<tEventActivity*>(fore)->SignalShutdown();
		};
		tSysResReal() :
				tSysRes()
		{
			in_shutdown.store(false);
			fore = new tEventActivity();
			AddDefaultDnsBase(*this);
			back = (new tThreadedActivity())->Spawn(*this);
			meta = (new tThreadedActivity())->Spawn(*this);
		}
	};
	return std::make_unique<tSysResReal>();
//	return std::unique_ptr<tSysRes>(new tSysResReal(move(ret)));
}

}
