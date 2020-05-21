#ifndef __EVABASE_H__
#define __EVABASE_H__

#include "meta.h"
#include "activity.h"

#include <event.h>

//struct evdns_base;

namespace acng
{

using tAutoEv = resource_owner<event*, event_free, nullptr>;
class tDnsBase;
class IDlConFactory;

// global access to lifecycle resources like thread contexts, and some cached service functionality
// normally created only once per application lifetime
struct ACNG_API tSysRes
{
	std::shared_ptr<tDnsBase> dnsbase;
	std::atomic<bool> in_shutdown;

	IDlConFactory* TcpConnectionFactory;

	// the main activity, run by main thread
	IActivity* fore;
	// the helper thread used for handling metadata (typically loading of DB records, file opening)
	IActivity* meta;
	// background thread, for best-effort tasks
	IActivity* back;

	virtual event_base * GetEventBase() {return nullptr;}
	virtual ~tSysRes() {}

	/**
	 * Runs the main loop for a program around the event_base loop.
	 * When finished, clean up some resources left behind (fire off specific events
	 * which have actions that would cause blocking otherwise).
	 */
	virtual int MainLoop() { return -1; };

	/**
	 * Tell the executing loop to cancel unfinished activities and stop.
	 */
	virtual void SignalShutdown() {};

};

// test code will have its own mock for this
std::unique_ptr<tSysRes> CreateRegularSystemResources();

}

#endif
