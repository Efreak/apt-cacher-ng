#ifndef CONSERVER_H_
#define CONSERVER_H_

#include "meta.h"

namespace acng
{

namespace conserver {

/*! Prepares the connection handlers and internal things, binds, listens, etc.
 * @return Number of created listeners.
 */
int Setup();
/// Start the service
int Run();
/// Stop all running connections sanely and threads if possible
ACNG_API void Shutdown();

void HandleOverload();

/**
 * Return connection handle for proper closing.
 */
void FinishConnection(int fd);

}

}

#endif /*CONSERVER_H_*/
