#ifndef SOCKIO_H_
#define SOCKIO_H_

#include "meta.h"
#include "acfg.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>

#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <unistd.h>
#include <cstddef>
#include <event.h>

#ifndef AI_NUMERICSERV
#define AI_NUMERICSERV 0
#endif
#ifndef AI_ADDRCONFIG
#define AI_ADDRCONFIG 0
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef MSG_MORE
#define MSG_MORE 0
#endif

#ifndef SO_MAXCONN
#define SO_MAXCONN 250
#endif

namespace acng
{

#ifdef HAVE_SSL
void globalSslInit();
#endif

void termsocket(int);

inline void termsocket_quick(int fd)
{
	if(fd<0)
		return;
	::shutdown(fd, SHUT_RDWR);
	while(0 != ::close(fd))
	{
		if(errno != EINTR) break;
	};
	fd=-1;
}

inline bool check_read_state(int fd)
{
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	struct timeval tv = { 0, 0};
	return (1 == select(fd + 1, &rfds, nullptr, nullptr, &tv) && FD_ISSET(fd, &rfds));
}

extern event_base *g_ebase;

inline const struct timeval& GetNetworkTimeout()
{
	static struct timeval tv;
	tv.tv_sec = cfg::nettimeout;
	tv.tv_usec = 23;
	return tv;
}
}

#endif /*SOCKIO_H_*/
