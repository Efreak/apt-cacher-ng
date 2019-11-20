#ifndef __EVABASE_H__
#define __EVABASE_H__

#include "config.h"
#include <event.h>
#include <evdns.h>

#define TEARDOWN_HINT short(0xffff)

namespace acng
{
namespace evabase
{
extern event_base *base;
extern evdns_base *dnsbase;
}

}

#endif
