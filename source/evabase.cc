/*
 * evabase.cc
 *
 *  Created on: 14.03.2019
 *      Author: EB
 */

#include "evabase.h"

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

}
