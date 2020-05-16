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
	event_base *base;
	tDnsBase* dnsbase;
	std::atomic<bool> in_shutdown;

	IDlConFactory* TcpConnectionFactory;

	// the main activity, run by main thread
	IActivity* fore;
	// the helper thread used for handling metadata (typically loading of DB records, file opening)
	IActivity* meta;
	// background thread, for best-effort tasks
	IActivity* back;

	virtual ~tSysRes() {}
};

// test code will have its own mock for this
std::unique_ptr<tSysRes> CreateRegularSystemResources();

}

#endif
