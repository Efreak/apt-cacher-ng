#ifndef __EVABASE_H__
#define __EVABASE_H__

#include "config.h"
#include <event.h>

struct evdns_base;

#define TEARDOWN_HINT short(0xffff)

namespace acng
{
/**
 * This class is an adapter for general libevent handling, roughly fitting it into conventions of the rest of ACNG.
 */
class ACNG_API evabase
{
public:
static event_base *base;
static evdns_base *dnsbase;
/**
 * Runs the main loop for a program around the event_base loop.
 * When finished, clean up some resources left behind (fire off specific events
 * which have actions that would cause blocking otherwise).
 */
int MainLoop();

static void SignalStop() {	if(evabase::base) event_base_loopbreak(evabase::base); }

evabase();
~evabase();
};

}

#endif
