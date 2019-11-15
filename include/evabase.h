#ifndef __EVABASE_H__
#define __EVABASE_H__

#include "config.h"
#include <event.h>

#define TEARDOWN_HINT short(0xffff)

namespace acng
{
namespace evabase
{
extern event_base *base;
}

}

#endif
