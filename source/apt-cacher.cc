

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
/* OpenSSL headers */

#include "openssl/bio.h"
#include "openssl/ssl.h"
#include "openssl/err.h"


#endif


#include "filereader.h"
#include "csmapping.h"
#ifdef DEBUG
#include <regex.h>
#endif

#include "maintenance.h"

static void usage();
static void SetupCacheDir();
void sig_handler(int signum);
void log_handler(int signum);
void dump_handler(int signum);
void check_algos();

//void DispatchAndRunMaintTask(cmstring &cmd, int fd, const char *auth);
int wcat(LPCSTR url, LPCSTR proxy);

typedef struct sigaction tSigAct;

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

void runDemo()
{

#ifdef DEBUG
	cerr << "Pandora: " << sizeof(regex_t) << endl;


	// PLAYGROUND

	/*
	 cerr << sizeof(job) << endl;
	 exit(1);
	 */

	/*
	 * Let's be another csum tool...

	 md5_state_s ctx;
	 md5_init(&ctx);
	 uint8_t buf[2000];
	 while(!feof(stdin))
	 {
	 int n=fread(buf, sizeof(char), 2000, stdin);
	 md5_append(&ctx, buf, n);
	 }
	 uint8_t csum[16];
	 md5_finish(&ctx, csum);
	 for(int i=0;i<16;i++)
	 printf("%02x", csum[i]);
	 printf("\n");
	 exit(0);



	if(argc<2)
		return -1;

	acfg::tHostInfo hi;
	cout << "Parsing " << argv[1] << ", result: " << hi.SetUrl(argv[1])<<endl;
	cout << "Host: " << hi.sHost <<", Port: " << hi.sPort << ", Path: " << hi.sPath<<endl;
	return 0;


	 bool Bz2compressFile(const char *, const char*);
	 return ! Bz2compressFile(argv[1], argv[2]);


	 char tbuf[40];
	 FormatCurrentTime(tbuf);
	 std::cerr << tbuf << std::endl;
	 exit(1);

*/

#endif
	if (getenv("GETSUM"))
	{
		uint8_t csum[20];
		string s(getenv("GETSUM"));
		off_t resSize;
		bool ok = filereader::GetChecksum(s, CSTYPE_SHA1, csum, true, resSize /*, stdout*/);
		for (UINT i = 0; i < sizeof(csum); i++)
			printf("%02x", csum[i]);
		printf("\n");
		if (ok && getenv("REFSUM"))
		{
			printf(CsEqual(getenv("REFSUM"), csum, sizeof(csum)) ? "IsOK\n" : "Diff\n");
		}
		exit(0);
	}

/*
	LPCSTR envvar = getenv("PARSEIDX");
	if (envvar)
	{
		int parseidx_demo(LPCSTR);
		exit(parseidx_demo(envvar));
	}

	if (getenv("SHRINK"))
	{
		uint8_t csum[20];
		string s(getenv("SHRINK"));
		off_t resSize;
		auto n=(filereader::GetChecksum(s, CSTYPE_SHA1, csum, true, resSize, stdout));
		exit(n);
	}
	*/
}


int main(int argc, char **argv)
{

#ifdef HAVE_SSL
	SSL_load_error_strings();
	ERR_load_BIO_strings();
	ERR_load_crypto_strings();
	ERR_load_SSL_strings();
	OpenSSL_add_all_algorithms();
	SSL_library_init();
#endif

	const char *envvar=getenv("TOBASE64");
	if(envvar)
	{
		std::cout << EncodeBase64Auth(envvar);
		return 0;
	}
	envvar=getenv("BECURL");
	if(envvar)
		return wcat(envvar, getenv("http_proxy"));

	check_algos();
	tSigAct act = tSigAct();

	sigfillset(&act.sa_mask);
	act.sa_handler = &sig_handler;
	sigaction(SIGBUS, &act, NULL);
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGINT, &act, NULL);
	sigaction(SIGQUIT, &act, NULL);

	act.sa_handler = &dump_handler;
	sigaction(SIGUSR2, &act, NULL);

	act.sa_handler = &log_handler;
	sigaction(SIGUSR1, &act, NULL);

	act.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &act, NULL);
#ifdef SIGIO
	sigaction(SIGIO, &act, NULL);
#endif
#ifdef SIGXFSZ
	sigaction(SIGXFSZ, &act, NULL);
#endif

//#ifdef DEBUG
	runDemo();
//#endif

	// preprocess some startup related parameters
	bool bForceCleanup(false);
	for (char **p=argv+1; p<argv+argc; p++)
	{
		if (!strncmp(*p, "-h", 2))
			usage();
		else if (!strncmp(*p, "-v", 2))
		{
			acfg::debug=acfg::debug|LOG_DEBUG|LOG_MORE;
			**p=0x0; // ignore it if ever checked anywhere
		}
		else if (!strncmp(*p, "-e", 2))
		{
			bForceCleanup=true;
			**p=0x0; // ignore it if ever checked anywhere
		}
	}

	LPCSTR PRINTCFGVAR=getenv("PRINTCFGVAR");
	bool bDumpCfg(false);

	for (char **p=argv+1; p<argv+argc; p++)
	{
		if(!strcmp(*p, "-c"))
		{
			++p;
			if(p < argv+argc)
				acfg::ReadConfigDirectory(*p, PRINTCFGVAR);
			else
				usage();
		}
		else if(!strcmp(*p, "-p"))
		{
			bDumpCfg=true;
		}
		else if(**p) // not empty
		{
			if(!acfg::SetOption(*p, false))
				usage();
		}
	}

	if(PRINTCFGVAR)
	{
		auto ps(acfg::GetStringPtr(PRINTCFGVAR));
		if(ps)
		{
			cout << *ps << endl;
			return EXIT_SUCCESS;
		}
		auto pi(acfg::GetIntPtr(PRINTCFGVAR));
		if(pi)
			cout << *pi << endl;
		return pi ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if(!aclog::open())
	{
		cerr << "Problem creating log files. Check permissions of the log directory, "
			<< acfg::logdir<<endl;
		exit(1);
	}

	acfg::PostProcConfig(bDumpCfg);

	if(bDumpCfg)
		exit(EXIT_SUCCESS);

	SetupCacheDir();

	extern mstring sReplDir;
	DelTree(acfg::cacheDirSlash+sReplDir);

	conserver::Setup();

	if (bForceCleanup)
	{
		tSpecialRequest::RunMaintWork(tSpecialRequest::workExExpire,
				acfg::reportpage + "?abortOnErrors=aOe&doExpire=Start",
				fileno(stdout));
		exit(0);
	}

	if (acfg::foreground)
		return conserver::Run();

	if (!fork_away())
	{
		tErrnoFmter ef("Failed to change to daemon mode");
		cerr << ef << endl;
		exit(43);
	}

	if (!acfg::pidfile.empty())
	{
		mkbasedir(acfg::pidfile);
		FILE *PID_FILE = fopen(acfg::pidfile.c_str(), "w");
		if (PID_FILE != NULL)
		{
			fprintf(PID_FILE, "%d", getpid());
			checkForceFclose(PID_FILE);
		}
	}

	return conserver::Run();

}

static void usage() {
	cout <<"Usage: apt-cacher -h -c configdir <var=value ...>\n\n"
		"Options:\n"
		"-h: this help message\n"
		"-c: configuration directory\n"
		"-e: on startup, run expiration once\n"
		"-p: print configuration and exit\n"
		"\n"
		"Most interesting variables:\n"
		"ForeGround: Don't detach (default: 0)\n"
		"Port: TCP port number (default: 3142)\n"
		"CacheDir: /directory/for/storage\n"
		"LogDir: /directory/for/logfiles\n"
		"\n"
		"See configuration examples for all directives.\n\n";
	exit(0);
}


static void SetupCacheDir()
{
	using namespace acfg;
	if(!Cstat(cacheDirSlash))
	{
		// well, attempt to create it then
		mstring path=cacheDirSlash+'/';
		for(UINT pos=0; (pos=path.find(SZPATHSEP, pos)) < path.size(); ++pos)
			mkdir((const char*) path.substr(0,pos).c_str(), (UINT) dirperms);
	}

	struct timeval tv;
	gettimeofday(&tv, NULL);
	tSS buf;
	buf << cacheDirSlash << "testfile." <<42*tv.tv_usec*tv.tv_sec;
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
#ifdef DEBUG
	cerr << "caught signal " << signum <<endl;
#endif
	switch (signum) {
	case (SIGBUS):
		/* OH NO!
		 * Something going wrong with the mmaped files.
		 * Log the current state and shutdown gracefully.
		 */

		void report_bad_mmap_state();
		report_bad_mmap_state();
		aclog::flush();

		// nope, not reliable yet, just exit ASAP and hope that systemd will restart us
		// return;
		signum = SIGTERM;

	case (SIGTERM):
	case (SIGINT):
	case (SIGQUIT): {
		g_victor.Stop();
		aclog::close(false);
		// and then terminate, resending the signal to default handler
		tSigAct act = tSigAct();
		sigfillset(&act.sa_mask);
		act.sa_handler = SIG_DFL;
		if (sigaction(signum, &act, NULL))
			abort(); // shouldn't be needed, but have a sane fallback in case
		raise(signum);
	}
	default:
		return;
	}
}

#include "dlcon.h"
#include "fileio.h"
#include "fileitem.h"

int wcat(LPCSTR surl, LPCSTR proxy)
{

	acfg::dnscachetime=0;
	acfg::persistoutgoing=0;

	if(proxy)
		if(acfg::proxy_info.SetHttpUrl(proxy))
			return -1;
	tHttpUrl url;
	if(!surl || !url.SetHttpUrl(surl))
		return -2;
	dlcon dl(true);

	class tPrintItem : public fileitem
	{
		public:
			tPrintItem()
			{
				m_bAllowStoreData=false;
				m_nSizeChecked = m_nSizeSeen = 0;
			};
			virtual FiStatus Setup(bool)
			{
				m_nSizeChecked = m_nSizeSeen = 0;
				return m_status = FIST_INITED;
			}
			virtual int GetFileFd() { return 1; }; // something, don't care for now
			virtual bool DownloadStartedStoreHeader(const header & h, const char *,
					bool, bool&)
			{
				return true;
			}
			virtual bool StoreFileData(const char *data, unsigned int size)
			{
				return (size==fwrite(data, sizeof(char), size, stdout));
			}
			ssize_t SendData(int , int, off_t &, size_t )
			{
				return 0;
			}
	};

	tFileItemPtr fi((fileitem*)new tPrintItem);
	dl.AddJob(fi, url);
	dl.WorkLoop();
	return (fi->WaitForFinish(NULL) == fileitem::FIST_COMPLETE
			&& fi->GetHeaderUnlocked().getStatus() == 200) ? 0 : -3;
}

