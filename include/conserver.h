#ifndef CONSERVER_H_
#define CONSERVER_H_


namespace acng
{

namespace conserver {

/*! Prepares the connection handlers and internal things, binds, listens, etc.
 * @return Nothing, uses exit to abort
 */
void Setup();
/// Start the service
int Run();
/// Stop all running connections sanely and threads if possible
void Shutdown();

}

}

#endif /*CONSERVER_H_*/
