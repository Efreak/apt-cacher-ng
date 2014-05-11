
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
#include <iostream>
#include <algorithm>    // std::min_element, std::max_element

#ifdef HAVE_LIBWRAP
#include <tcpd.h>
#endif

#include "debug.h"

using namespace MYSTD;

// for cygwin, incomplete ipv6 support
#ifndef AF_INET6
#define AF_INET6        23              /* IP version 6 */
#endif


namespace conserver
{


int g_sockunix(-1);
vector<int> g_vecSocks;

condition g_ThreadPoolCondition;
list<con*> g_freshConQueue;
int g_nStandbyThreads(0);
int g_nAllConThreadCount(0);
bool bTerminationMode(false);

// safety mechanism, detect when the number of incoming connections
// is growing A LOT faster than it can be processed 
#define MAX_BACKLOG 200

void * ThreadAction(void *)
{
	lockguard g(g_ThreadPoolCondition);
	list<con*> & Qu = g_freshConQueue;

	while (true)
	{
		while (Qu.empty() && !bTerminationMode)
			g_ThreadPoolCondition.wait();

		if (bTerminationMode)
			break;

		con *c=Qu.front();
		Qu.pop_front();

		g_nStandbyThreads--;
		g.unLock();
	
		c->WorkLoop();
		delete c;

		g.reLock();
		g_nStandbyThreads++;

		if (g_nStandbyThreads >= acfg::tpstandbymax)
			break;
	}
	
	g_nAllConThreadCount--;
	g_nStandbyThreads--;

	return NULL;
}

bool CreateDetachedThread(void *(*__start_routine)(void *))
{
	pthread_t thr;
	pthread_attr_t attr; // be detached from the beginning

	if (pthread_attr_init(&attr))
		return false;
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	bool bOK = (0 == pthread_create(&thr, &attr, __start_routine, NULL));
	pthread_attr_destroy(&attr);
	return bOK;
}

//! pushes waiting thread(s) and create threads for each waiting task if needed
inline bool SpawnThreadsAsNeeded()
{
	lockguard g(g_ThreadPoolCondition);
	list<con*> & Qu = g_freshConQueue;

	// check the kill-switch
	if(g_nAllConThreadCount+1>=acfg::tpthreadmax || bTerminationMode)
		return false;

	int nNeeded = Qu.size()-g_nStandbyThreads;
	
	while (nNeeded-- > 0)
	{
		if (!CreateDetachedThread(ThreadAction))
			return false;
		g_nStandbyThreads++;
		g_nAllConThreadCount++;
	}

	g_ThreadPoolCondition.notifyAll();

	return true;
}


void SetupConAndGo(int fd, const char *szClientName=NULL)
{
	LOGSTART2s("SetupConAndGo", fd);

	if(!szClientName)
		szClientName="";
	
	USRDBG( "Client name: " << szClientName);
	con *c(NULL);

	{
		// thread pool control, and also see Shutdown(), protect from
		// interference of OS on file descriptor management
		lockguard g(g_ThreadPoolCondition);

		// DOS prevention
		if (g_freshConQueue.size() > MAX_BACKLOG)
		{
			USRDBG( "Worker queue overrun");
			goto local_con_failure;
		}

		try
		{
			c = new con(fd, szClientName);
			if (!c)
			{
#ifdef NO_EXCEPTIONS
				USRDBG( "Out of memory");
#endif
				goto local_con_failure;
			}

			g_freshConQueue.push_back(c);
			LOG("Connection to backlog, total count: " << g_freshConQueue.size());


		} catch (MYSTD::bad_alloc&)
		{
			USRDBG( "Out of memory");
			goto local_con_failure;
		}
	}
	
	if (!SpawnThreadsAsNeeded())
	{
		errnoFmter fer("Cannot start threads, cleaning up. Reason: ");
		USRDBG(fer);
		lockguard g(g_ThreadPoolCondition);
		while(!g_freshConQueue.empty())
		{
			delete g_freshConQueue.back();
			g_freshConQueue.pop_back();
		}
	}

	return;

	local_con_failure:
	if (c)
		delete c;
	USRDBG( "Connection setup error");
	termsocket_quick(fd);
}

void CreateUnixSocket() {
	string & sPath=acfg::fifopath;
	auto addr_unx = sockaddr_un();
	
	int jo=1;
	size_t size = sPath.length()+1+offsetof(struct sockaddr_un, sun_path);
	
	if(sPath.length()>sizeof(addr_unx.sun_path))
	{
		errno=ENAMETOOLONG;
		goto unx_err;
	}
	
	addr_unx.sun_family = AF_UNIX;
	strncpy(addr_unx.sun_path, sPath.c_str(), sPath.length());
	
	mkbasedir(sPath);
	unlink(sPath.c_str());
	
	g_sockunix = socket(PF_UNIX, SOCK_STREAM, 0);
	setsockopt(g_sockunix, SOL_SOCKET, SO_REUSEADDR, &jo, sizeof(jo));

	if (g_sockunix<0)
		goto unx_err;
	
	if (::bind(g_sockunix, (struct sockaddr *)&addr_unx, size) < 0)
		goto unx_err;
	
	if (0==listen(g_sockunix, SO_MAXCONN))
	{
		g_vecSocks.push_back(g_sockunix);
		return;
	}
	
	unx_err:
	cerr << "Error creating Unix Domain Socket, ";
	cerr.flush();
	perror(acfg::fifopath.c_str());
	cerr << "Check socket file and directory permissions" <<endl;
	exit(EXIT_FAILURE);

}

void Setup()
{
	LOGSTART2s("Setup", 0);
	using namespace acfg;
	
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
		int yes(1);
		
		tStrVec sAdds;
		if(bindaddr.empty())
			sAdds.push_back(""); // one dummy entry to get one NULL later
		else
			Tokenize(bindaddr, SPACECHARS, sAdds);
		for(tStrVec::iterator it=sAdds.begin(); it!=sAdds.end(); it++)
		{
			trimString(*it);
		    struct addrinfo *res, *p;
		    
		    if(0!=getaddrinfo(bindaddr.empty() ? NULL : it->c_str(),
				   port.c_str(), &hints, &res))
			   goto error_getaddrinfo;
		    
		    for(p=res; p; p=p->ai_next)
		    {
		    	int nSockFd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
				if (nSockFd<0)
					goto error_socket;

				// if we have a dual-stack IP implementation (like on Linux) then
				// explicitely disable the shadow v4 listener. Otherwise it might be
				// bound, or maybe not, and then maybe because of the dual-behaviour,
				// or maybe because of real errors; we just cannot know for sure but
				// we have to.
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
		    	g_vecSocks.push_back(nSockFd);
		    	
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
		    continue;
		    
		    error_getaddrinfo:
		    perror("Error resolving address for binding");
		}
		if(g_vecSocks.empty())
		{
			cerr << "No socket(s) could be created/prepared. "
			"Check the network, check or unset the BindAddress directive.\n";
			exit(EXIT_FAILURE);
		}
	}
	else
		aclog::err("Not creating TCP listening socket, no valid port specified!");

	if ( !acfg::fifopath.empty() )
		CreateUnixSocket();
	else
		aclog::err("Not creating Unix Domain Socket, fifo_path not specified");
}

int Run()
{
	LOGSTART2s("Run", "GoGoGo");
	fd_set rfds, wfds;
	int maxfd = 1 + *max_element(g_vecSocks.begin(), g_vecSocks.end());
	USRDBG( "Listening to incoming connections...");

	while (1)
	{ // main accept() loop

		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		for(vector<int>::iterator it=g_vecSocks.begin(); it!=g_vecSocks.end(); it++)
			FD_SET(*it, &rfds);
		
		//cerr << "Polling..." <<endl;
		int nReady=select(maxfd, &rfds, &wfds, NULL, NULL);
		if (nReady<0)
		{
			if(errno == EINTR)
				continue;
			
			aclog::err("select", "failure");
			perror("Select died");
			exit(EXIT_FAILURE);
		}
		
		for(vector<int>::iterator it=g_vecSocks.begin(); it!=g_vecSocks.end(); it++)
		{
			if(!FD_ISSET(*it, &rfds)) 	continue;

			if(g_sockunix == *it)
			{
				int fd = accept(g_sockunix, NULL, NULL);
				if (fd>=0)
				{
					set_nb(fd);
					USRDBG( "Detected incoming connection from the UNIX socket");
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
				int fd=accept(*it,(struct sockaddr *)&addr, &addrlen);
//fd_accepted:
				if (fd>=0)
				{
					set_nb(fd);
					char hbuf[NI_MAXHOST];
					USRDBG( "Detected incoming connection from the TCP socket");

					if (getnameinfo((struct sockaddr*) &addr, addrlen, hbuf, sizeof(hbuf),
									NULL, 0, NI_NUMERICHOST))
					{
						aclog::err("ERROR: could not resolve hostname for incoming TCP host");
						termsocket_quick(fd);
						sleep(1);
						continue;
					}

					if (acfg::usewrap)
					{
#ifdef HAVE_LIBWRAP
						// libwrap is non-reentrant stuff, call it from here only
						request_info req;
						request_init(&req, RQ_DAEMON, "apt-cacher-ng", RQ_FILE, fd, 0);
						fromhost(&req);
						if (!hosts_access(&req))
						{
							aclog::err("ERROR: access not permitted by hosts files", hbuf);
							termsocket_quick(fd);
							continue;
						}
#else
						aclog::err("WARNING: attempted to use libwrap which was not enabled at build time");
#endif
					}

					SetupConAndGo(fd, hbuf);
				}
				else if(errno == EMFILE || errno == ENOMEM || ENOBUFS == errno)
				{
					// play nicely, give it a break
					sleep(1);
				}
			}
		}
	}
	return 0;
}

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
	for(vector<int>::iterator it=g_vecSocks.begin(); it!=g_vecSocks.end(); it++)
		termsocket_quick(*it);
}

}
