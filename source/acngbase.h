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
class IDbManager;

// global access to lifecycle resources like thread contexts, and some cached service functionality
// normally created only once per application lifetime
class ACNG_API tSysRes
{
public:
	std::shared_ptr<tDnsBase> dnsbase;

	// the main activity, run by main thread
	std::unique_ptr<IActivity> fore;
	// the helper thread used for handling metadata (typically loading of DB records, file opening)
	std::unique_ptr<IActivity> meta;
	// background thread, for best-effort tasks
	std::unique_ptr<IActivity> back;

	std::shared_ptr<IDbManager> db = nullptr;
	IDlConFactory* TcpConnectionFactory = nullptr;

	virtual event_base * GetEventBase() {return nullptr;}

	// predefine a reliable shutdown order,, even for partly mocked versions
	virtual ~tSysRes()
	{
		// BG threads shall no longer accept jobs and prepare to exit ASAP
		if(back) back->StartShutdown();
		if(meta) meta->StartShutdown();
		dnsbase.reset();
		// and make sure to stop IO, however this is probably already interrupted from the loopbreak callback
		if(fore) fore->StartShutdown();
	}

	/**
	 * Runs the main loop for a program around the event_base loop.
	 * When finished, clean up some resources left behind (fire off specific events
	 * which have actions that would cause blocking otherwise).
	 */
	virtual int MainLoop() { return -1; };
};

// test code will have its own mock for this
std::unique_ptr<tSysRes> CreateRegularSystemResources();

}

#endif
