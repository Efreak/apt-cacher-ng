
#include "debug.h"

#include <config.h>
#include "lockable.h"

#include <chrono>

namespace acng
{
bool base_with_condition::wait_until(lockuniq& uli, time_t nUTCsecs, long msec)
{
	auto tpUTC = std::chrono::system_clock::from_time_t(nUTCsecs);
	tpUTC += std::chrono::milliseconds(msec);
	auto r = m_obj_cond.wait_until(uli._guard, tpUTC);
	return std::cv_status::timeout == r;
}

bool base_with_condition::wait_for(lockuniq& uli, long secs, long msec)
{
	auto r = m_obj_cond.wait_for(uli._guard, std::chrono::milliseconds(msec + secs*1000));
	return std::cv_status::timeout == r;
}

}
