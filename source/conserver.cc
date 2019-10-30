
#include "conserver.h"
#include "meta.h"
#include "lockable.h"
#include "conn.h"
#include "acfg.h"
#include "caddrinfo.h"
#include "sockio.h"
#include "fileio.h"
#include "evabase.h"
#include "evasocket.h"
#include "dnsiter.h"
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

#include <event2/event.h>

#include "debug.h"

using namespace std;

// for cygwin, incomplete ipv6 support
#ifndef AF_INET6
#define AF_INET6        23              /* IP version 6 */
#endif

namespace acng
{

namespace conserver
{

int yes(1);
vector<acng::event_socket> g_vecSocks;

base_with_condition g_ThreadPoolCondition;
list<conn*> g_freshConQueue;
int g_nStandbyThreads(0);
int g_nAllConThreadCount(0);
bool bTerminationMode(false);

void SetupConAndGo(int fd, const char *szClientName);

// safety mechanism, detect when the number of incoming connections
// is growing A LOT faster than it can be processed 
#define MAX_BACKLOG 200

void * ThreadAction(void *)
{
	lockuniq g(g_ThreadPoolCondition);
	list<conn*> & Qu = g_freshConQueue;

	while (true)
	{
		while (Qu.empty() && !bTerminationMode)
			g_ThreadPoolCondition.wait(g);

		if (bTerminationMode)
			break;

		conn *c=Qu.front();
		Qu.pop_front();

		g_nStandbyThreads--;
		g.unLock();
	
		c->WorkLoop();
		delete c;

		g.reLock();
		g_nStandbyThreads++;

		if (g_nStandbyThreads >= cfg::tpstandbymax)
			break;
	}
	
	g_nAllConThreadCount--;
	g_nStandbyThreads--;

	return nullptr;
}

bool CreateDetachedThread(void *(*__start_routine)(void *))
{
	pthread_t thr;
	pthread_attr_t attr; // be detached from the beginning

	if (pthread_attr_init(&attr))
		return false;
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	bool bOK = (0 == pthread_create(&thr, &attr, __start_routine, nullptr));
	pthread_attr_destroy(&attr);
	return bOK;
}

//! pushes waiting thread(s) and create threads for each waiting task if needed
inline bool SpawnThreadsAsNeeded()
{
	lockguard g(g_ThreadPoolCondition);
	list<conn*> & Qu = g_freshConQueue;

	// check the kill-switch
	if(g_nAllConThreadCount+1>=cfg::tpthreadmax || bTerminationMode)
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

void do_accept(const std::shared_ptr<evasocket>& soc)
{
	LOGSTART2s("do_accept", soc->fd());

	struct sockaddr_storage addr;
	socklen_t addrlen = sizeof(addr);

	int fd = accept(soc->fd(), (struct sockaddr *) &addr, &addrlen);
	auto_raii<int, forceclose> err_clean(fd, -1);

	if (fd >= 0)
	{
		set_nb(fd);
		if (addr.ss_family == AF_UNIX)
		{
			USRDBG("Detected incoming connection from the UNIX socket");
			SetupConAndGo(fd, nullptr);
			err_clean.disable();
		}
		else
		{
			USRDBG("Detected incoming connection from the TCP socket");
			char hbuf[NI_MAXHOST];
			if (getnameinfo((struct sockaddr*) &addr, addrlen, hbuf, sizeof(hbuf), nullptr,
					0, NI_NUMERICHOST))
			{
				log::err("ERROR: could not resolve hostname for incoming TCP host");
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
					return;
				}
#else
				log::err("WARNING: attempted to use libwrap which was not enabled at build time");
#endif
			}
			SetupConAndGo(fd, hbuf);
			err_clean.disable();
		}
	}
}

void SetupConAndGo(int fd, const char *szClientName)
{
	LOGSTART2s("SetupConAndGo", fd);

	if(!szClientName)
		szClientName="";
	
	USRDBG( "Client name: " << szClientName);
	conn *c(nullptr);

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
			c = new conn(fd, szClientName);
			if (!c)
			{
#ifdef NO_EXCEPTIONS
				USRDBG( "Out of memory");
#endif
				goto local_con_failure;
			}

			g_freshConQueue.emplace_back(c);
			LOG("Connection to backlog, total count: " << g_freshConQueue.size());


		} catch (std::bad_alloc&)
		{
			USRDBG( "Out of memory");
			goto local_con_failure;
		}
	}
	
	if (!SpawnThreadsAsNeeded())
	{
		tErrnoFmter fer("Cannot start threads, cleaning up. Reason: ");
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

auto bind_and_listen =
		[](shared_ptr<evasocket> mSock, const sockaddr* addi, unsigned int addilen) -> bool
		{
			if ( ::bind(mSock->fd(), addi, addilen))
			{
				perror("Couldn't bind socket");
				cerr.flush();
				if(EADDRINUSE == errno)
				cerr << "Port " << cfg::port << " is busy, see the manual (Troubleshooting chapter) for details." <<endl;
				cerr.flush();
				return false;
			}
			if (listen(mSock->fd(), SO_MAXCONN))
			{
				perror("Couldn't listen on socket");
				return false;
			}

			g_vecSocks.emplace_back(evabase::instance,
					mSock,
					EV_READ | EV_PERSIST,
					[](const std::shared_ptr<evasocket>& sock, short) {do_accept(sock);});
			// and activate it once
			g_vecSocks.back().enable();
			return true;
		};

std::string scratchBuf;

auto setup_tcp_listeners = [](LPCSTR addi, const std::string& port) -> unsigned
{
	LOGSTART2s("Setup::ConAddr", 0);

	CAddrInfo resolver;
	auto hints = evutil_addrinfo();
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = PF_UNSPEC;

	if(!resolver.ResolveTcpTarget(addi ? addi : sEmptyString, port, scratchBuf, &hints))
	{
		perror("Error resolving address for binding");
		return 0;
	}

	std::unordered_set<std::string> dedup;
	tDnsIterator iter(PF_UNSPEC, resolver.getTcpAddrInfo());
	unsigned res(0);
	for(const evutil_addrinfo *p; !!(p=iter.next());)
	{
		// no fit or or seen before?
		if((p->ai_family != AF_INET6 && p->ai_family != AF_INET) ||
				!dedup.insert(std::string((LPCSTR)p->ai_addr, p->ai_addrlen)).second)
		{
			continue;
		}

// managed socket
		shared_ptr<evasocket> mSock;

		int nSockFd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (nSockFd == -1)
		{
			// STFU on lag of IPv6?
			switch(errno)
			{
				case EAFNOSUPPORT:
				case EPFNOSUPPORT:
				case ESOCKTNOSUPPORT:
				case EPROTONOSUPPORT:
				continue;
				default:
				perror("Error creating socket");
				continue;
			}
		}
		mSock = evasocket::create(nSockFd);

// if we have a dual-stack IP implementation (like on Linux) then
// explicitly disable the shadow v4 listener. Otherwise it might be
// bound or maybe not, and then just sometimes because of configurable
// dual-behavior, or maybe because of real errors;
// we just cannot know for sure but we need to.
#if defined(IPV6_V6ONLY) && defined(SOL_IPV6)
		if(p->ai_family==AF_INET6) setsockopt(mSock->fd(), SOL_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
#endif
		setsockopt(mSock->fd(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
		res += bind_and_listen(mSock, p->ai_addr, p->ai_addrlen);
	}
	return res;
};

int ACNG_API Setup()
{
	LOGSTART2s("Setup", 0);
	
	if (cfg::fifopath.empty() && cfg::port.empty())
	{
		cerr << "Neither TCP nor UNIX interface configured, cannot proceed.\n";
		exit(EXIT_FAILURE);
	}

	unsigned nCreated = 0;

	if (cfg::fifopath.empty())
		log::err("Not creating Unix Domain Socket, fifo_path not specified");
	else
	{
		string & sPath = cfg::fifopath;
		auto addr_unx = sockaddr_un();

		size_t size = sPath.length() + 1 + offsetof(struct sockaddr_un, sun_path);

		auto die = []()
		{
			cerr << "Error creating Unix Domain Socket, ";
			cerr.flush();
			perror(cfg::fifopath.c_str());
			cerr << "Check socket file and directory permissions" <<endl;
			exit(EXIT_FAILURE);
		};

		if (sPath.length() > sizeof(addr_unx.sun_path))
		{
			errno = ENAMETOOLONG;
			die();
		}

		addr_unx.sun_family = AF_UNIX;
		strncpy(addr_unx.sun_path, sPath.c_str(), sPath.length());

		mkbasedir(sPath);
		unlink(sPath.c_str());

		auto sockFd = socket(PF_UNIX, SOCK_STREAM, 0);
		if(sockFd < 0) die();
		nCreated += bind_and_listen(evasocket::create(sockFd), (struct sockaddr *) &addr_unx, size);
	}

	if (atoi(cfg::port.c_str()) <= 0)
		log::err("Not creating TCP listening socket, no valid port specified!");
	else
	{
		bool custom_listen_ip = false;
		for(const auto& sp: tSplitWalk(&cfg::bindaddr))
		{
			nCreated += setup_tcp_listeners(sp.c_str(), cfg::port);
			custom_listen_ip = true;
		}
		// just TCP_ANY if none was specified
		if(!custom_listen_ip)
			nCreated += setup_tcp_listeners(nullptr, cfg::port);
	}
	return nCreated;
}

int ACNG_API Run()
{
	LOGSTART2s("Run", "GoGoGo");

#ifdef HAVE_SD_NOTIFY
	sd_notify(0, "READY=1");
#endif

	return event_base_loop(evabase::instance->base, EVLOOP_NO_EXIT_ON_EMPTY);
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
	// terminate activities
	g_vecSocks.clear();
}

}

}
