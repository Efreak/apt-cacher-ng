#ifndef __EVABASE_H__
#define __EVABASE_H__

#include "config.h"

struct event_base;
#include <memory>
#include <unordered_set>

namespace acng
{

class socket_activity_base;

/**
 * Event lib abstraction - destructible event base
 */
class ACNG_API evabase
{
	std::unordered_set<socket_activity_base*> m_weak_ref_users;

public:
	event_base *base;
	static std::atomic<bool> in_shutdown;
	/** Share the global instance created by main() */
	static std::shared_ptr<evabase> instance;
	evabase();
	~evabase();

	// add a "free running" activity reference to the collection, for later shutdown
	void register_activity(socket_activity_base*);
	void unregister_activity(socket_activity_base*);
	void invoke_shutdown_activities();
};

}

#endif
