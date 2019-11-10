#include <memory>

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
vector<SHARED_PTR<acng::event_socket> > g_vecSocks;

base_with_condition g_thread_push_cond_var;
deque<unique_ptr<conn>> g_freshConQueue;
unsigned g_nStandbyThreads = 0, g_nTotalThreads=0;

bool g_suspended = false; // temporary suspended operation
SHARED_PTR<acng::event_socket> g_resumer;

void SetupConAndGo(unique_fd fd, const char *szClientName);
void do_resume();

// safety mechanism, detect when the number of incoming connections
// is growing A LOT faster than it can be processed 
#define MAX_BACKLOG 200

void do_accept(const std::shared_ptr<evasocket>& soc)
{
	LOGSTART2s("do_accept", soc->fd());

	struct sockaddr_storage addr;
	socklen_t addrlen = sizeof(addr);

	int fd = -1;
	while(true)
	{
		fd = accept(soc->fd(), (struct sockaddr*) &addr, &addrlen);

		if (fd != -1)
			break;

		switch (errno)
		{
		case EAGAIN:
		case EINTR:
			continue;
		case EMFILE:
		case ENFILE:
		case ENOBUFS:
		case ENOMEM:
			// resource exhaustion, might recover when another connection handler has stopped, disconnect this one for now
			conserver::HandleOverload();
			return;
		default:
			return;
		}
	}

	unique_fd man_fd(fd);

	evutil_make_socket_nonblocking(fd);
	if (addr.ss_family == AF_UNIX)
	{
		USRDBG("Detected incoming connection from the UNIX socket");
		SetupConAndGo(move(man_fd), nullptr);
	}
	else
	{
		USRDBG("Detected incoming connection from the TCP socket");
		char hbuf[NI_MAXHOST];
		if (getnameinfo((struct sockaddr*) &addr, addrlen, hbuf, sizeof(hbuf),
				nullptr, 0, NI_NUMERICHOST))
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
			log::err(
					"WARNING: attempted to use libwrap which was not enabled at build time");
#endif
		}
		SetupConAndGo(move(man_fd), hbuf);
	}
}


auto ThreadAction = []()
{
	lockuniq g(g_thread_push_cond_var);

	while (true)
	{
		while (g_freshConQueue.empty() && !g_global_shutdown)
			g_thread_push_cond_var.wait(g);

		if (g_global_shutdown)
			break;

		auto c = move(g_freshConQueue.front());
		g_freshConQueue.pop_front();

		g_nStandbyThreads--;

		g.unLock();
		c->WorkLoop();
		c.reset();
		g.reLock();

		g_nStandbyThreads++;

		if (int(g_nStandbyThreads) >= cfg::tpstandbymax || g_global_shutdown)
			break;
	}

	// remove from global pool
	g_nStandbyThreads--;
	g_nTotalThreads--;
	g_thread_push_cond_var.notifyAll();
};

//! pushes waiting thread(s) and create threads for each waiting task if needed
auto SpawnThreadsAsNeeded = []()
{
	// check the kill-switch
		if(int(g_nTotalThreads+1)>=cfg::tpthreadmax || g_global_shutdown)
		return false;

	// need a custom one
		if(g_nStandbyThreads == 0)
		{
			try
			{
				thread thr(ThreadAction);
				// if thread was started w/o exception it will decrement those in the end
				g_nStandbyThreads++;
				g_nTotalThreads++;
				thr.detach();
			}
			catch(...)
			{
				return false;
			}
		}
		g_thread_push_cond_var.notifyAll();
		return true;
	};

void SetupConAndGo(unique_fd man_fd, const char *szClientName)
{
	LOGSTART2s("SetupConAndGo", man_fd.get());

	if (!szClientName)
		szClientName = "";


	USRDBG("Client name: " << szClientName);

	lockguard g(g_thread_push_cond_var);

	// DOS prevention
	if (g_freshConQueue.size() > MAX_BACKLOG)
	{
		USRDBG("Worker queue overrun");
		return;
	}

	try
	{
		g_freshConQueue.emplace_back(make_unique<conn>(move(man_fd), szClientName));
		LOG("Connection to backlog, total count: " << g_freshConQueue.size());
	}
	catch (const std::bad_alloc&)
	{
		USRDBG("Out of memory");
		return;
	}

	if (!SpawnThreadsAsNeeded())
	{
		tErrnoFmter fer(
				"Cannot start threads, cleaning up, aborting all incoming connections. Reason: ");
		USRDBG(fer);
		g_freshConQueue.clear();
	}
}

bool bind_and_listen(shared_ptr<evasocket> mSock, const evutil_addrinfo *pAddrInfo)
		{
	LOGSTART2s("bind_and_listen", formatIpPort(pAddrInfo));
			if ( ::bind(mSock->fd(), pAddrInfo->ai_addr, pAddrInfo->ai_addrlen))
			{
				log::flush();
				perror("Couldn't bind socket");
				cerr.flush();
				if(EADDRINUSE == errno)
				{
					if(pAddrInfo->ai_family == PF_UNIX)
						cerr << "Error creating or binding the UNIX domain socket - please check permissions!" <<endl;
					else
						cerr << "Port " << cfg::port << " is busy, see the manual (Troubleshooting chapter) for details." <<endl;
				cerr.flush();
				}
				return false;
			}
			if (listen(mSock->fd(), SO_MAXCONN))
			{
				perror("Couldn't listen on socket");
				return false;
			}

			g_vecSocks.emplace_back(
					make_shared<event_socket>(evabase::instance,
					mSock,
					EV_READ | EV_PERSIST,
					[](const std::shared_ptr<evasocket>& sock, short) {do_accept(sock);})
					);
			// and activate it once
			g_vecSocks.back()->enable();
			return true;
		};

std::string scratchBuf;

unsigned setup_tcp_listeners(LPCSTR addi, const std::string& port)
{
	LOGSTART2s("Setup::ConAddr", 0);

	CAddrInfo resolver;
	auto hints = evutil_addrinfo();
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = PF_UNSPEC;
	bool ignored;
	if(!resolver.ResolveTcpTarget(addi ? addi : sEmptyString, port, scratchBuf, &hints, ignored))
	{
		log::flush();
		perror("Error resolving address for binding");
		return 0;
	}

	std::unordered_set<std::string> dedup;
	tDnsIterator iter(PF_UNSPEC, resolver.getTcpAddrInfo());
	unsigned res(0);
	for(const evutil_addrinfo *p; !!(p=iter.next());)
	{
		// no fit or or seen before?
		if(!dedup.emplace((const char*) p->ai_addr, p->ai_addrlen).second)
			continue;
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
		auto mSock = evasocket::create(nSockFd);
		// if we have a dual-stack IP implementation (like on Linux) then
		// explicitly disable the shadow v4 listener. Otherwise it might be
		// bound or maybe not, and then just sometimes because of configurable
		// dual-behavior, or maybe because of real errors;
		// we just cannot know for sure but we need to.
#if defined(IPV6_V6ONLY) && defined(SOL_IPV6)
		if(p->ai_family==AF_INET6)
			setsockopt(mSock->fd(), SOL_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
#endif
		setsockopt(mSock->fd(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
		res += bind_and_listen(mSock, p);
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

	{
		lockguard g(g_thread_push_cond_var);
		g_resumer = std::make_shared<event_socket>(evabase::instance, evasocket::create(-1), 0,
				[](const std::shared_ptr<evasocket>&, short)
				{	do_resume();});
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

		evutil_addrinfo ai;
		ai.ai_addr =(struct sockaddr *) &addr_unx;
		ai.ai_addrlen = size;
		ai.ai_family = PF_UNIX;

		nCreated += bind_and_listen(evasocket::create(sockFd), &ai);
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
	DBGQLOG("Closing listening sockets\n");
	// terminate activities
	g_vecSocks.clear();
	g_resumer.reset();

	{
		lockuniq g(g_thread_push_cond_var);
		// global hint to all conn objects
		g_global_shutdown = true;
		DBGQLOG("Notifying worker threads\n");
		g_thread_push_cond_var.notifyAll();
		while(g_nTotalThreads)
			g_thread_push_cond_var.wait(g);
	}
}

const struct timeval g_resumeTimeout { 2, 11 };

void HandleOverload()
{
	lockguard g(g_thread_push_cond_var);
	g_suspended = true;
	for(auto& it: g_vecSocks)
	{
		it->disable();
	}
	g_resumer->enable(&g_resumeTimeout);
}

void do_resume()
{
	lockguard g(g_thread_push_cond_var);
	if(!g_suspended) return;
	if(!g_resumer) return; // already in shutdown phase
	g_resumer->disable();
	for(auto& it: g_vecSocks)
	{
		it->enable();
	}
	g_suspended = false;
}

void FinishConnection(int fd)
{
	if(fd == -1)
		return;

	termsocket_async(fd, evabase::instance->base);
	// there is a good chance that more resources are available now
	do_resume();
}

}

}
