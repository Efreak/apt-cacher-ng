#ifndef __EVABASE_H__
#define __EVABASE_H__

#include "meta.h"
#include <event.h>

struct evdns_base;

namespace acng
{
/**
 * This class is an adapter for general libevent handling, roughly fitting it into conventions of the rest of ACNG.
 * Partly static and partly dynamic, for pure convenience! Expected to be a singleton anyway.
 */
class ACNG_API evabase
{
public:
static event_base *base;
static evdns_base *dnsbase;
static std::atomic<bool> in_shutdown;

/**
 * Runs the main loop for a program around the event_base loop.
 * When finished, clean up some resources left behind (fire off specific events
 * which have actions that would cause blocking otherwise).
 */
int MainLoop();

static void SignalStop();

using tCancelableAction = std::function<void(bool)>;

/**
 * Push an action into processing queue. In case operation is not possible, runs the action with the cancel flag (bool argument set to true)
 */
static void Post(tCancelableAction&&);

evabase();
~evabase();
};

}

#endif
