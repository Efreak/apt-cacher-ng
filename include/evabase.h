#ifndef __EVABASE_H__
#define __EVABASE_H__

#include "meta.h"
#include "activity.h"

#include <event.h>

//struct evdns_base;

namespace acng
{

using tAutoEv = resource_owner<event*, event_free, nullptr>;

// global resource access to lifecycle things like thread contexts
// normally created only once per application lifetime
class ACNG_API tSysRes
{
public:
	event_base *base;
	evdns_base *dnsbase;
	// the only public static element, becase we need this flag to be as easy accessible as possible
	std::atomic<bool> in_shutdown;

	// the main activity, run by main thread
	std::unique_ptr<IActivity> fore;
	// the helper thread used for handling metadata (typically loading of DB records, file opening)
	std::unique_ptr<IActivity> meta;
	// background thread, for best-effort tasks
	std::unique_ptr<IActivity> back;
};

}

#endif
