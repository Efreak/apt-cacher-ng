
#include "conserver.h"
#include "meta.h"
#include "lockable.h"
#include "conn.h"
#include "rfc2553emu.h"
#include "acfg.h"

#include "sockio.h"
#include "fileio.h"
#include <signal.h>
#include <arpa/inet.h>
#include <errno.h>

#include <netdb.h>

#include <cstdio>
#include <list>
#include <map>
#include <unordered_set>
#include <iostream>
#include <algorithm>    // std::min_element, std::max_element

#ifdef HAVE_LIBWRAP
#include <tcpd.h>
#endif

#ifdef HAVE_SD_NOTIFY
#include <systemd/sd-daemon.h>
#endif

#include "debug.h"

using namespace std;

// for cygwin, incomplete ipv6 support
#ifndef AF_INET6
#define AF_INET6        23              /* IP version 6 */
#endif

namespace acng
{
event_base *g_ebase = 0;

namespace conserver
{

int yes(1);

//int g_sockunix(-1);
//vector<int> g_vecSocks;

bool bTerminationMode(false);

void cb_conn(evutil_socket_t fd, short what, void *arg)
{
	if(!arg) return;
	auto c = (conn*) arg;

	// if frozen, close, if exited without continuation wish, close
	if( (what & EV_TIMEOUT) || (what = c->socketAction(fd, what), !what) )
	{
		delete c;
		termsocket(fd);
	}
	// thread is not dead, just waiting and will be activated by others again
	if(what&EV_SIGNAL)
		return;
#warning need this hack? where?

//	c->m_event->ev_flags

	event_add(c->m_event, & GetNetworkTimeout());
}

void SetupConAndGo(int fd, const char *szClientName=nullptr)
{
	LOGSTART2s("SetupConAndGo", fd);

	if (!szClientName)
		szClientName = "";

	USRDBG("Client name: " << szClientName);
	conn *c(nullptr);
	struct event *connEvent(nullptr);
	// delicate shutdown...
	auto clean_conn = [&]()
	{
		USRDBG("Out of memory");
		if (c)
		{
			c->m_event = nullptr;
			delete c;
		}
		if(connEvent)
		event_free(connEvent);
		termsocket_quick(fd);
	};
	try
	{
		c = new conn(szClientName);
		if (!c) // weid...
			throw std::bad_alloc();

		connEvent = event_new(g_ebase, fd, EV_READ, cb_conn, c);
		if(!connEvent)
			return clean_conn();
		c->m_event = connEvent;
		if(event_add(connEvent, &GetNetworkTimeout()))
			return clean_conn();
	}
	catch (...)
	{
		return clean_conn();
	}
}

void cb_accept(evutil_socket_t fdHot, short what, void *arg)
{
	if(arg)
	{
		int fd = accept(fdHot, nullptr, nullptr);
		if (fd>=0)
		{
			set_nb(fd);
//			USRDBG( "Detected incoming connection from the UNIX socket");
			SetupConAndGo(fd);
		}
		else if(errno == EMFILE || errno == ENOMEM || ENOBUFS == errno)
		{
			// play nicely, give it a break
			sleep(1);
		}
	}
	else
	{
		struct sockaddr_storage addr;

		socklen_t addrlen = sizeof(addr);
		int fd=accept(fdHot,(struct sockaddr *)&addr, &addrlen);
//fd_accepted:
		if (fd>=0)
		{
			set_nb(fd);
			char hbuf[NI_MAXHOST];
	//		USRDBG( "Detected incoming connection from the TCP socket");

			if (getnameinfo((struct sockaddr*) &addr, addrlen, hbuf, sizeof(hbuf),
							nullptr, 0, NI_NUMERICHOST))
			{
				log::err("ERROR: could not resolve hostname for incoming TCP host");
				termsocket_quick(fd);
				return;
			}

			if (cfg::usewrap)
			{
#ifdef HAVE_LIBWRAP
				// libwrap is non-reentrant stuff, call it from here only
				request_info req;
				request_init(&req, RQ_DAEMON, "apt-cacher-ng", RQ_FILE, fd, 0);
				fromhost(&req);
				if (!hosts_access(&req))
				{
					log::err("ERROR: access not permitted by hosts files", hbuf);
					termsocket_quick(fd);
					return;
				}
#else
				log::err("WARNING: attempted to use libwrap which was not enabled at build time");
#endif
			}

			SetupConAndGo(fd, hbuf);
		}
	}
}

void CreateUnixSocket() {
	string & sPath=cfg::fifopath;
	auto addr_unx = sockaddr_un();
	
	size_t size = sPath.length()+1+offsetof(struct sockaddr_un, sun_path);
	
	auto die=[]() {
		cerr << "Error creating Unix Domain Socket, ";
		cerr.flush();
		perror(cfg::fifopath.c_str());
		cerr << "Check socket file and directory permissions" <<endl;
		exit(EXIT_FAILURE);
	};

	if(sPath.length()>sizeof(addr_unx.sun_path))
	{
		errno=ENAMETOOLONG;
		die();
	}
	
	addr_unx.sun_family = AF_UNIX;
	strncpy(addr_unx.sun_path, sPath.c_str(), sPath.length());
	
	mkbasedir(sPath);
	unlink(sPath.c_str());
	
	int fd = socket(PF_UNIX, SOCK_STREAM, 0);
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	if (fd<0)
		die();
	
	if (::bind(fd, (struct sockaddr *)&addr_unx, size) < 0 || listen(fd, SO_MAXCONN))
		die();
	
	auto uev = event_new(g_ebase, fd, EV_READ | EV_PERSIST, cb_accept, (void*) 1);
	event_add(uev, nullptr);
}

void Setup()
{
	LOGSTART2s("Setup", 0);
	using namespace cfg;
	
	int nCreated=0;

	if (fifopath.empty() && port.empty())
	{
		cerr << "Neither TCP nor UNIX interface configured, cannot proceed.\n";
		exit(EXIT_FAILURE);
	}
	
	if (atoi(port.c_str())>0)
	{
		auto hints = addrinfo();
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE;
		hints.ai_family = 0;
		
		auto conaddr = [hints, &nCreated](LPCSTR addi)
		{
			LOGSTART2s("Setup::ConAddr", 0);

		    struct addrinfo *res, *p;
		    if(0!=getaddrinfo(addi, port.c_str(), &hints, &res))
		    {
		    	perror("Error resolving address for binding");
		    	return;
		    }

		    std::unordered_set<std::string> dedup;
		    for(p=res; p; p=p->ai_next)
		    {
		    	if(p->ai_family != AF_INET6 && p->ai_family != AF_INET)
		    		continue;

		    	// processed before?
		    	if(!dedup.insert(std::string((LPCSTR)p->ai_addr, p->ai_addrlen)).second)
		    		continue;

		    	int nSockFd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
				if (nSockFd<0)
					goto error_socket;

				// if we have a dual-stack IP implementation (like on Linux) then
				// explicitly disable the shadow v4 listener. Otherwise it might be
				// bound or maybe not, and then just sometimes because of configurable
				// dual-behavior, or maybe because of real errors;
				// we just cannot know for sure but we need to.
#if defined(IPV6_V6ONLY) && defined(SOL_IPV6)
				if(p->ai_family==AF_INET6)
					setsockopt(nSockFd, SOL_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
#endif
				setsockopt(nSockFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
				
		    	if (::bind(nSockFd, p->ai_addr, p->ai_addrlen))
		    		goto error_bind;
		    	if (listen(nSockFd, SO_MAXCONN))
		    		goto error_listen;
		    	
		    	USRDBG( "created socket, fd: " << nSockFd);// << ", for bindaddr: "<<bindaddr);

		    	event_add(event_new(g_ebase, nSockFd, EV_READ | EV_PERSIST, cb_accept, (void*) 0), nullptr);
		    	nCreated++;

		    	continue;

				error_socket:
				
				if(EAFNOSUPPORT != errno &&
						EPFNOSUPPORT != errno &&
						ESOCKTNOSUPPORT != errno &&
						EPROTONOSUPPORT != errno)
				{
					perror("Error creating socket");
				}
				goto close_socket;

				error_listen:
				perror("Couldn't listen on socket");
				goto close_socket;

				error_bind:
				perror("Couldn't bind socket");
				cerr.flush();
				if(EADDRINUSE == errno)
					cerr << "Port " << port << " is busy, see the manual (Troubleshooting chapter) for details." <<endl;
				cerr.flush();
				goto close_socket;

				close_socket:
				forceclose(nSockFd);
		    }
		    freeaddrinfo(res);
		};

		tSplitWalk sp(&bindaddr);
		if(sp.Next())
		{
			do
			{
				conaddr(sp.str().c_str());
			}
			while(sp.Next());
		}
		else
			conaddr(nullptr);

		if(!nCreated)
		{
			cerr << "No socket(s) could be created/prepared. "
			"Check the network, check or unset the BindAddress directive.\n";
			exit(EXIT_FAILURE);
		}
	}
	else
		log::err("Not creating TCP listening socket, no valid port specified!");

	if ( !cfg::fifopath.empty() )
	{
		CreateUnixSocket();
		nCreated++;
	}
	else
		log::err("Not creating Unix Domain Socket, fifo_path not specified");

	if(!nCreated)
	{
		cerr << "No valid server sockets configured" <<endl;
		exit(EXIT_FAILURE);
	}

}

int Run()
{
	LOGSTART2s("Run", "GoGoGo");

#ifdef HAVE_SD_NOTIFY
	sd_notify(0, "READY=1");
#endif

	USRDBG( "Listening to incoming connections...");

	event_base_loop(g_ebase, 0);
	return 0;
}

#if 0
void Shutdown()
{
	lockguard g(g_ThreadPoolCondition);
	
	if(bTerminationMode)
		return; // double SIGWHATEVER? Prevent it.
	
	//for (map<con*,int>::iterator it=mConStatus.begin(); it !=mConStatus.end(); it++)
	//	it->first->SignalStop();
	// TODO: maybe call shutdown on all?
	//printf("Signaled stop to all cons\n");
	
	bTerminationMode=true;
	printf("Notifying waiting threads\n");
	g_ThreadPoolCondition.notifyAll();
	
	printf("Closing listening sockets\n");

	event_base_loopbreak(g_ebase);

#warning will be easier when threads deliverd in event thread
	#warning FIXME: how to monitor when the last callback exited? we are currently in a signal handler on some random thread
	//for(auto soc: g_vecSocks) termsocket_quick(soc);
#warning after exit, close all sockets
}
#endif

}

/*
 * Will be added for cleaner shutdown with future libevent version, for now: don't care
int cb_shutdown_event_socket(const struct event_base *,
    const struct event *pEv, void *)
{
	if(pEv)	termsocket_quick(pEv->ev_fd);
}
*/
void shutdownAllSockets()
{
//	event_base_foreach_event(acng::g_ebase, cb_shutdown_event_socket,0);
}

}

