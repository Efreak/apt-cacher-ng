/*
 * tlsio.h
 *
 *  Created on: 23.12.2019
 *      Author: Eduard Bloch
 */

#ifndef INCLUDE_TLSIO_H_
#define INCLUDE_TLSIO_H_

#include "config.h"

#ifdef HAVE_SSL
#include <openssl/ssl.h>
#endif

namespace acng
{

// helper class which needs to be instantiated once per address space
namespace atls
{

#ifdef HAVE_SSL
	void ACNG_API Init();
	void ACNG_API Deinit();
	SSL_CTX* GetContext();
	std::string GetInitError();
#else
#error FIXME, add dummies
#endif

};

}

#endif /* INCLUDE_TLSIO_H_ */
