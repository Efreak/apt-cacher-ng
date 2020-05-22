#include "acngbase.h"
#include "meta.h"
#include "debug.h"
#include "lockable.h"
#include "dbman.h"

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

const struct timeval timeout_asap{0,0};


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

class CActivityBase : public IActivity
{
protected:
	deque<tCancelableAction> incoming_q;
	std::thread::id mainThreadId;
	mutex handover_mx;
	bool m_shutdown = false;

	virtual void InstallUnderLock(tCancelableAction&& act) =0;
public:
	/*
	 * Like Post, but if the thread is the same as main thread, run the action in-place
	 */
	JobAcceptance PostOrRun(tCancelableAction&& act) override
	{
		if(!act)
			return NOT_ACCEPTED;

		if (this_thread::get_id() == mainThreadId)
		{
			{
				std::unique_lock g(handover_mx);
				if (m_shutdown)
					return NOT_ACCEPTED;
			}
			act(false);
			return FINISHED;
		}
		return Post(move(act));
	}

	/**
	 * Push an action into processing queue. In case operation is not possible, runs the action with the cancel flag (bool argument set to true)
	 */
	JobAcceptance Post(tCancelableAction&& act) override
	{
		if(!act)
			return NOT_ACCEPTED;
		{
			lockguard g(handover_mx);
			if(m_shutdown)
				return NOT_ACCEPTED;
			InstallUnderLock(move(act));
		}
		return ACCEPTED;
	}

	std::thread::id GetThreadId() override
	{
		return mainThreadId;
	}

};

class tEventActivity : public CActivityBase
{

public:
	event_base *m_base = nullptr;
	struct event *handover_wakeup;

	static void cb_handover(evutil_socket_t sock, short what, void* arg)
	{
		bool last_cycle = false, have_more=false;
		auto me((tEventActivity*)arg);
		tCancelableAction todo;
		// pick just one job per cycle, unless in shutdown phase, then run them ASAP
		{
			lockguard g(me->handover_mx);
			last_cycle = me->m_shutdown;
			have_more = me->incoming_q.size()>1;
			todo.swap(me->incoming_q.front());
			me->incoming_q.pop_front();
		}
		if(todo) todo(last_cycle);
		if(have_more && !last_cycle)
			event_add(me->handover_wakeup, &timeout_asap);


#if 0
		while(true)
		{
			{
				lockguard g(me->handover_mx);
				last_cycle = m_shutdown;
				me->temp_q.clear();
				me->temp_q.swap(me->incoming_q);
			}
			for (const auto &ac : me->temp_q)
			{
				if(ac)
					ac(false);
			}
			me->temp_q.clear();
			if (!last_cycle)
			{

			}

		}
#endif
	}

	void InstallUnderLock(tCancelableAction&& act) override
	{
		incoming_q.emplace_back(move(act));
		if(incoming_q.size() == 1)
			event_add(handover_wakeup, &timeout_asap);
	}
	void StartShutdown() override
	{
		std::lock_guard<std::mutex> g(handover_mx);
		m_shutdown = true;
		event_base_loopbreak(m_base);
	}

	tEventActivity()
	{
		evthread_use_pthreads();
		m_base = event_base_new();
		handover_wakeup =  evtimer_new(m_base, cb_handover, this);
	}
	~tEventActivity()
	{
		// just once to finalize outstanding tasks
		event_base_loop(m_base, EVLOOP_ONCE | EVLOOP_NONBLOCK);
		event_base_free(m_base);
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
};

class tThreadedActivity : public CActivityBase
{
	std::thread m_thread;
	std::condition_variable waitvar;
public:

	// start a new background thread, make sure that it became operational when exiting
	IActivity* Spawn()
	{
		std::promise<bool> confirm_start;
		std::thread([&]()
				{
			decltype(incoming_q) temp_q;
			mainThreadId = this_thread::get_id();
			confirm_start.set_value(true);

			while (true)
			{
				bool last_cycle = false;

				{
					std::unique_lock<std::mutex> q(handover_mx);
					temp_q.clear();
					while(incoming_q.empty())
						waitvar.wait(q);
					last_cycle = m_shutdown;
					temp_q.swap(incoming_q);
				}
				for (const auto &ac : temp_q)
				{
					if(ac)
						ac(last_cycle);
				}

				if(last_cycle)
					break;
			}
		}).swap(m_thread);

		return confirm_start.get_future().get() ? this : nullptr;
	}

	void StartShutdown() override
	{
		std::lock_guard<std::mutex> g(handover_mx);
		m_shutdown = true;
		// just push it, the thread will stop after
		incoming_q.emplace_back(tCancelableAction());
		waitvar.notify_one();
	}

	~tThreadedActivity()
	{
		if(m_thread.joinable())
			m_thread.join();

		// XXX: is flushing here the safest approach?
		decltype(incoming_q) temp_q;
		{
			std::unique_lock<std::mutex> q(handover_mx);
			temp_q.swap(incoming_q);
		}
		for (const auto &ac : temp_q)
		{
			if(ac)
				ac(true);
		}
		temp_q.clear();
	}

	void InstallUnderLock(tCancelableAction&& act) override
	{
		incoming_q.emplace_back(move(act));
		waitvar.notify_one();
	}
};

void AddDefaultDnsBase(tSysRes &src);

std::unique_ptr<tSysRes> CreateRegularSystemResources()
{
	struct tSysResReal : public tSysRes
	{
		inline tEventActivity* eac() {return static_cast<tEventActivity*>(this->fore);}
		event_base * GetEventBase() override
		{
			return eac()->m_base;
		}
		~tSysResReal()
		{
			// BG threads shall no longer accept jobs and prepare to exit ASAP
			back->StartShutdown();
			meta->StartShutdown();
			fore->StartShutdown();
			// finalize all outstanding work
			delete back;
			delete meta;
			delete fore;
			delete db;
		}
		int MainLoop() override
		{
			return eac()->MainLoop();
		}
		tSysResReal() :
				tSysRes()
		{
			fore = new tEventActivity();
			AddDefaultDnsBase(*this);
			back = (new tThreadedActivity())->Spawn();
			meta = (new tThreadedActivity())->Spawn();
		}
	};
	return std::make_unique<tSysResReal>();
//	return std::unique_ptr<tSysRes>(new tSysResReal(move(ret)));
}

}
