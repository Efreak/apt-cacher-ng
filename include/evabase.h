#ifndef __EVABASE_H__
#define __EVABASE_H__

#include "meta.h"

struct event_base;

//int main(int argc, const char** argv);

namespace acng
{

//class socket_activity;

/**
 * Event lib abstraction - destructible event base
 */
class ACNG_API evabase
{
public:
	event_base *base;
	/** Share the global instance created by main() */
	static std::shared_ptr<evabase> instance;
	evabase();
	~evabase();

	// tell that activity is being deleted
	// void notify_activity_destroying(socket_activity*);
};

}

#endif
