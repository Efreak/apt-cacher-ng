#include <acbuf.h>
#include <aclogger.h>
#include <fcntl.h>

#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstdbool>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>

#define LOCAL_DEBUG
#include "debug.h"

#include "config.h"
#include "meta.h"
#include "acfg.h"
#include "fileio.h"
#include "conserver.h"
#include "cleaner.h"
#include "lockable.h"

using namespace std;

#include <iostream>
#include <cstdio>
#include <cstring>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>

#include "filereader.h"
#include "csmapping.h"
#ifdef DEBUG
#include <regex.h>
#endif

#include "maintenance.h"

std::deque<std::function< void() > > g_serviceQ;
acng::base_with_condition g_serviceLockable;
std::thread g_serviceThread;
bool g_serviceStopSignal { true };
acng::tEventFd g_serviceNotifier;

void serviceResultNotify()
{
	g_serviceNotifier.eventPoke();
}

void enqueServiceAction(std::function< void() > action)
{
	acng::lockguard g(g_serviceLockable);
	g_serviceQ.emplace_back(action);
	g_serviceLockable.notifyAll();
}

void stopServiceThread()
{
	acng::lockuniq g(g_serviceLockable);
	g_serviceStopSignal = true;
	g_serviceLockable.notifyAll();
	g.unLock();
	g_serviceThread.join();
}

namespace acng
{

//pthread_t g_event_thread;

static void usage(int nRetCode=0);
static void SetupCacheDir();
void handle_sigbus();
void check_algos();

extern mstring sReplDir;

typedef struct sigaction tSigAct;

cleaner g_victor;

#ifdef HAVE_DAEMON
inline bool fork_away()
{
	return !daemon(0,0);
}
#else
inline bool fork_away()
{
	chdir("/");
	int dummy=open("/dev/null", O_RDWR);
	if(0<=dup2(dummy, fileno(stdin))
			&& 0<=dup2(dummy, fileno(stdout))
			&& 0<=dup2(dummy, fileno(stderr)))
	{
		switch(fork())
		{
			case 0: // this is child, good
				return true;
			case -1: // bad...
				return false;
			default: // in parent -> cleanup
				setsid();
				_exit(0);
		}
	}
	return false;
}
#endif

void parse_options(int argc, const char **argv, bool& bStartCleanup)
{
	bool bExtraVerb=false;
	LPCSTR szCfgDir=nullptr;
	std::vector<LPCSTR> cmdvars;
	bool ignoreCfgErrors = false;

	for (auto p=argv+1; p<argv+argc; p++)
	{
		if (!strncmp(*p, "--", 2))
			break;
		if (!strncmp(*p, "-h", 2))
			usage();
		if (!strncmp(*p, "-i", 2))
			ignoreCfgErrors = true;
		else if (!strncmp(*p, "-v", 2))
			bExtraVerb = true;
		else if (!strncmp(*p, "-e", 2))
			bStartCleanup=true;
		else if (!strcmp(*p, "-c"))
		{
			++p;
			if (p < argv + argc)
				szCfgDir = *p;
			else
				usage(2);
		}
		else if(**p) // not empty
			cmdvars.emplace_back(*p);
	}

	if(szCfgDir)
		cfg::ReadConfigDirectory(szCfgDir, !ignoreCfgErrors);

	for(auto& keyval : cmdvars)
		if(!cfg::SetOption(keyval, 0))
			usage(EXIT_FAILURE);

	cfg::PostProcConfig();

	if(bExtraVerb)
		cfg::debug |= (log::LOG_DEBUG|log::LOG_MORE);

}

void disable_signals()
{
	tSigAct act = tSigAct();
	sigfillset(&act.sa_mask);
	act.sa_handler = SIG_IGN;
	sigaction(SIGBUS, &act, nullptr);
	sigaction(SIGTERM, &act, nullptr);
	sigaction(SIGINT, &act, nullptr);
	sigaction(SIGQUIT, &act, nullptr);
	sigaction(SIGUSR2, &act, nullptr);
	sigaction(SIGUSR1, &act, nullptr);
	sigaction(SIGPIPE, &act, nullptr);
#ifdef SIGIO
	sigaction(SIGIO, &act, nullptr);
#endif
#ifdef SIGXFSZ
	sigaction(SIGXFSZ, &act, nullptr);
#endif
}

static void usage(int retCode) {
	cout <<"Usage: apt-cacher-ng [options] [ -c configdir ] <var=value ...>\n\n"
		"Options:\n"
		"-h: this help message\n"
		"-c: configuration directory\n"
		"-e: on startup, run expiration once\n"
		"-p: print configuration and exit\n"
		"-i: ignore configuration loading errors\n"
#if SUPPWHASH
		"-H: read a password from STDIN and print its hash\n"
#endif
		"\n"
		"Most interesting variables:\n"
		"ForeGround: Don't detach (default: 0)\n"
		"Port: TCP port number (default: 3142)\n"
		"CacheDir: /directory/for/storage\n"
		"LogDir: /directory/for/logfiles\n"
		"\n"
		"See configuration examples for all directives.\n\n";
	exit(retCode);
}


static void SetupCacheDir()
{
	using namespace cfg;

	if(cfg::cachedir.empty())
		return;	// warning was printed

	auto xstore(cacheDirSlash + cfg::privStoreRelSnapSufix);
	mkdirhier(xstore);
	if(!Cstat(xstore))
	{
		cerr << "Error: Cannot create any directory in " << cacheDirSlash << endl;
		exit(EXIT_FAILURE);
	}
	mkdirhier(cacheDirSlash + cfg::privStoreRelQstatsSfx + "/i");
	mkdirhier(cacheDirSlash + cfg::privStoreRelQstatsSfx + "/o");
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	tSS buf;
	buf << cacheDirSlash << "testfile." << tv.tv_usec * tv.tv_sec * (LPCSTR(buf.wptr()) - LPCSTR(&tv));
	mkbasedir(buf.c_str()); // try or force its directory creation
	int t=open( buf.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 00644);
	if (t>=0)
	{
		forceclose(t);
		if(0==unlink(buf.c_str()))
			return;
	}
	cerr << "Failed to create cache directory or directory not writable." << endl
		<< "Check the permissions of " << cachedir << "!" << endl;

	exit(1);
}


void cb_signal(evutil_socket_t signum, short what, void *arg)
{
	dbgprint("caught signal " << signum);
	switch (signum) {

	case SIGUSR2:
		void dump_handler();
		return dump_handler();

	case SIGUSR1:
		return log::close(true);

	case (SIGBUS):
		/* OH NO!
		 * Something going wrong with the mmaped files.
		 * Log the current state reliably.
		 * As long as there is no good recovery mechanism,
		 * just hope that systemd will restart the daemon.
		 */
		handle_sigbus();
		log::flush();
		//no break
	case (SIGTERM):
	case (SIGINT):
	case (SIGQUIT):
	{
		stopServiceThread();
		void shutdownAllSockets();
		shutdownAllSockets();
		g_victor.Stop();
		log::close(false);
		if (!cfg::pidfile.empty())
			unlink(cfg::pidfile.c_str());
		exit(signum != SIGQUIT);
	}
	}
}

}

int main(int argc, const char **argv)
{
	using namespace acng;
#ifdef HAVE_SSL
	acng::globalSslInit();
#endif

	check_algos();

	SetupCacheDir();

	DelTree(cfg::cacheDirSlash+sReplDir);


	g_ebase = event_base_new();
	if(!g_serviceNotifier.setupEventFd())
	{
		cerr << "Error creating basic file descriptors" <<endl;
		exit(1);
	}

	disable_signals();

	g_serviceThread = std::thread {
		[]() {
			acng::lockuniq ug(g_serviceLockable);
			g_serviceStopSignal = false;

			while(!g_serviceStopSignal)
			{
				if(g_serviceQ.empty())
				{
					g_serviceLockable.wait(ug);
					continue;
				}
				auto todo = g_serviceQ.front();
				g_serviceQ.pop_front();
				ug.unLock();
				todo();
			}
		}
	};
	{
		acng::lockuniq ug(g_serviceLockable);
		while(g_serviceStopSignal) g_serviceLockable.wait(ug);
	}

	bool bRunCleanup=false;

#ifdef DEBUG
	for(int i=0; i<argc; ++i)
		if(0==strcmp(argv[i], "~~"))
			argc=i++;
#endif

	parse_options(argc, argv, bRunCleanup);

	if(!log::open())
	{
		cerr << "Problem creating log files. Check permissions of the log directory, "
			<< cfg::logdir<<endl;
		exit(EXIT_FAILURE);
	}

	conserver::Setup();

	for(auto signum: {SIGBUS, SIGTERM, SIGINT, SIGQUIT, SIGUSR1, SIGUSR2})
		event_add(evsignal_new(g_ebase, signum, cb_signal, NULL), 0);

	if (bRunCleanup)
	{
		tSpecialRequest::RunMaintWork(tSpecialRequest::workExExpire,
				cfg::reportpage + "?abortOnErrors=aOe&doExpire=Start",
				fileno(stdout));
		exit(0);
	}

	if (!cfg::foreground && !fork_away())
	{
		tErrnoFmter ef("Failed to change to daemon mode");
		cerr << ef << endl;
		exit(43);
	}

	if (!cfg::pidfile.empty())
	{
		mkbasedir(cfg::pidfile);
		FILE *PID_FILE = fopen(cfg::pidfile.c_str(), "w");
		if (PID_FILE != nullptr)
		{
			fprintf(PID_FILE, "%d", getpid());
			checkForceFclose(PID_FILE);
		}
	}
	return conserver::Run();

}
