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


#define LOCAL_DEBUG
#include "debug.h"

#include "config.h"
#include "meta.h"
#include "acfg.h"
#include "fileio.h"
#include "conserver.h"
#include "cleaner.h"

#include <iostream>
using namespace std;

#include <cstdio>
#include <cstring>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>

#ifdef HAVE_SSL
#include <openssl/evp.h>
#include "openssl/bio.h"
#include "openssl/ssl.h"
#include "openssl/err.h"
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#endif

#include "filereader.h"
#include "csmapping.h"
#ifdef DEBUG
#include <regex.h>
#endif

#include "maintenance.h"

static void usage(int nRetCode=0);
static void SetupCacheDir();
void sig_handler(int signum);
void log_handler(int signum);
void dump_handler(int signum);
void handle_sigbus();
void check_algos();

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
		acfg::ReadConfigDirectory(szCfgDir, !ignoreCfgErrors);

	for(auto& keyval : cmdvars)
		if(!acfg::SetOption(keyval, 0))
			usage(EXIT_FAILURE);

	acfg::PostProcConfig();

	if(bExtraVerb)
		acfg::debug |= (LOG_DEBUG|LOG_MORE);

}

void setup_sighandler()
{
	tSigAct act = tSigAct();

	sigfillset(&act.sa_mask);
	act.sa_handler = &sig_handler;
	sigaction(SIGBUS, &act, nullptr);
	sigaction(SIGTERM, &act, nullptr);
	sigaction(SIGINT, &act, nullptr);
	sigaction(SIGQUIT, &act, nullptr);

	act.sa_handler = &dump_handler;
	sigaction(SIGUSR2, &act, nullptr);

	act.sa_handler = &log_handler;
	sigaction(SIGUSR1, &act, nullptr);

	act.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &act, nullptr);
#ifdef SIGIO
	sigaction(SIGIO, &act, nullptr);
#endif
#ifdef SIGXFSZ
	sigaction(SIGXFSZ, &act, nullptr);
#endif
}

int main(int argc, const char **argv)
{

#ifdef HAVE_SSL
	SSL_load_error_strings();
	ERR_load_BIO_strings();
	ERR_load_crypto_strings();
	ERR_load_SSL_strings();
	OpenSSL_add_all_algorithms();
	SSL_library_init();
#endif

	bool bRunCleanup=false;

	parse_options(argc, argv, bRunCleanup);

	if(!aclog::open())
	{
		cerr << "Problem creating log files. Check permissions of the log directory, "
			<< acfg::logdir<<endl;
		exit(EXIT_FAILURE);
	}

	check_algos();
	setup_sighandler();

	SetupCacheDir();

	extern mstring sReplDir;
	DelTree(acfg::cacheDirSlash+sReplDir);

	conserver::Setup();

	if (bRunCleanup)
	{
		tSpecialRequest::RunMaintWork(tSpecialRequest::workExExpire,
				acfg::reportpage + "?abortOnErrors=aOe&doExpire=Start",
				fileno(stdout));
		exit(0);
	}

	if (!acfg::foreground && !fork_away())
	{
		tErrnoFmter ef("Failed to change to daemon mode");
		cerr << ef << endl;
		exit(43);
	}

	if (!acfg::pidfile.empty())
	{
		mkbasedir(acfg::pidfile);
		FILE *PID_FILE = fopen(acfg::pidfile.c_str(), "w");
		if (PID_FILE != nullptr)
		{
			fprintf(PID_FILE, "%d", getpid());
			checkForceFclose(PID_FILE);
		}
	}
	return conserver::Run();

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
	using namespace acfg;
	auto xstore(cacheDirSlash + "_xstore");
	mkbasedir(xstore + "/Release");
	if(!Cstat(xstore))
	{
		cerr << "Error: Cannot create any directory in " << cacheDirSlash << endl;
		exit(EXIT_FAILURE);
	}

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

void log_handler(int)
{
	aclog::close(true);
}

void sig_handler(int signum)
{
	dbgprint("caught signal " << signum);
	switch (signum) {
	case (SIGBUS):
		/* OH NO!
		 * Something going wrong with the mmaped files.
		 * Log the current state reliably.
		 * As long as there is no good recovery mechanism,
		 * just hope that systemd will restart the daemon.
		 */
		handle_sigbus();
		aclog::flush();
		//no break
	case (SIGTERM):
	case (SIGINT):
	case (SIGQUIT): {
		g_victor.Stop();
		aclog::close(false);
    if (!acfg::pidfile.empty())
       unlink(acfg::pidfile.c_str());
		// and then terminate, resending the signal to default handler
		tSigAct act = tSigAct();
		sigfillset(&act.sa_mask);
		act.sa_handler = SIG_DFL;
		if (sigaction(signum, &act, nullptr))
			abort(); // shouldn't be needed, but have a sane fallback in case
		raise(signum);
	}
	default:
		return;
	}
}
