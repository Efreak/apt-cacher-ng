/*
 * evabase.cc
 *
 *  Created on: 14.03.2019
 *      Author: EB
 */

#include "evabase.h"
#include "evasocket.h"

#include "meta.h"
#include <event.h>

namespace acng
{

std::shared_ptr<evabase> evabase::instance;

evabase::evabase() : base (event_base_new())
{
}

evabase::~evabase()
{
	event_base_free(base);
}

void acng::evabase::register_activity(socket_activity_base* p)
{
	m_weak_ref_users.insert(p);
}

void acng::evabase::unregister_activity(socket_activity_base* p)
{
	m_weak_ref_users.erase(p);
}

void evabase::invoke_shutdown_activities()
{
	auto snapshot = m_weak_ref_users;
	for(const auto& opfer: snapshot)
		if(opfer) opfer->invoke_shutdown();
}

}
