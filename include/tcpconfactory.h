/*
 * tcpconfactory.h
 *
 *  Created on: 15.12.2019
 *      Author: Eduard Bloch
 */

#ifndef INCLUDE_TCPCONFACTORY_H_
#define INCLUDE_TCPCONFACTORY_H_

#include "confactory.h"

namespace acng
{
class tSysRes;
IDlConFactory* CreateTcpConFactory(tSysRes* resman);
}

#endif /* INCLUDE_TCPCONFACTORY_H_ */
