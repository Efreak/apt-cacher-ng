/*
 * tcpconfactory.h
 *
 *  Created on: 15.12.2019
 *      Author: Eduard Bloch
 */

#ifndef INCLUDE_TCPCONFACTORY_H_
#define INCLUDE_TCPCONFACTORY_H_

#include "tcpconnect.h"
#include "confactory.h"

namespace acng
{
IDlConFactory& GetTcpConFactory();
}

#endif /* INCLUDE_TCPCONFACTORY_H_ */
