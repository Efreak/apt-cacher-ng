#include <acbuf.h>
#include <aclogger.h>
#include <fcntl.h>
#include <openssl/evp.h>
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
/* OpenSSL headers */
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

#if SUPPWHASH

int hashpwd()
{
#ifdef HAVE_SSL
	string plain;
	uint32_t salt=0;
	for(uint i=10; i; --i)
	{
		if(RAND_bytes(reinterpret_cast<unsigned char*>(&salt), 4) >0)
			break;
		else
			salt=0;
		sleep(1);
	}
	if(!salt) // ok, whatever...
	{
		uintptr_t pval = reinterpret_cast<uintptr_t>(&plain);
		srandom(uint(time(0)) + uint(pval) +uint(getpid()));
		salt=random();
		timespec ts;
		clock_gettime(CLOCK_BOOTTIME, &ts);
		for(auto c=(ts.tv_nsec+ts.tv_sec)%1024 ; c; c--)
			salt=random();
	}
	string crypass = BytesToHexString(reinterpret_cast<const uint8_t*>(&salt), 4);
#ifdef DEBUG
	plain="moopa";
#else
	cin >> plain;
#endif
	trimString(plain);
	if(!AppendPasswordHash(crypass, plain.data(), plain.size()))
		return EXIT_FAILURE;
	cout << crypass <<endl;
	return EXIT_SUCCESS;
#else
	cerr << "OpenSSL not available, hashing functionality disabled." <<endl;
	return EXIT_FAILURE;
#endif
}


bool AppendPasswordHash(string &stringWithSalt, LPCSTR plainPass, size_t passLen)
{
	if(stringWithSalt.length()<8)
		return false;

	uint8_t sum[20];
	if(1!=PKCS5_PBKDF2_HMAC_SHA1(plainPass, passLen,
			(unsigned char*) (stringWithSalt.data()+stringWithSalt.size()-8), 8,
			NUM_PBKDF2_ITERATIONS,
			sizeof(sum), (unsigned char*) sum))
		return false;
	stringWithSalt+=EncodeBase64((LPCSTR)sum, 20);
	stringWithSalt+="00";
#warning dbg
	// checksum byte
	uint8_t pCs=0;
	for(char c : stringWithSalt)
		pCs+=c;
	stringWithSalt+=BytesToHexString(&pCs, 1);
	return true;
}
#endif

void do_stuff_before_config()
{
	LPCSTR envvar(nullptr);

#ifdef DEBUG
	cerr << "Pandora: " << sizeof(regex_t) << endl;
	/*
	// PLAYGROUND
	if (argc < 2)
		return -1;

	acfg::tHostInfo hi;
	cout << "Parsing " << argv[1] << ", result: " << hi.SetUrl(argv[1]) << endl;
	cout << "Host: " << hi.sHost << ", Port: " << hi.sPort << ", Path: "
			<< hi.sPath << endl;
	return 0;

	bool Bz2compressFile(const char *, const char*);
	return !Bz2compressFile(argv[1], argv[2]);

	char tbuf[40];
	FormatCurrentTime(tbuf);
	std::cerr << tbuf << std::endl;
	exit(1);
	*/
	envvar = getenv("PARSEIDX");
	if (envvar)
	{
		int parseidx_demo(LPCSTR);
		exit(parseidx_demo(envvar));
	}
#endif

	envvar = getenv("GETSUM");
	if (envvar)
	{
		uint8_t csum[20];
		string s(envvar);
		off_t resSize;
		bool ok = filereader::GetChecksum(s, CSTYPE_SHA1, csum, false, resSize /*, stdout*/);
		if(!ok)
		{
			perror("");
			exit(1);
		}
		for (uint i = 0; i < sizeof(csum); i++)
			printf("%02x", csum[i]);
		printf("\n");
		envvar = getenv("REFSUM");
		if (ok && envvar)
		{
			if(CsEqual(envvar, csum, sizeof(csum)))
			{
				printf("IsOK\n");
				exit(0);
			}
			else
			{
				printf("Diff\n");
				exit(1);
			}
		}
		exit(0);
	}

	envvar=getenv("TOBASE64");
	if(envvar)
	{
		std::cout << EncodeBase64Auth(envvar);
		exit(EXIT_SUCCESS);
	}
	envvar=getenv("BECURL");
	if(envvar)
		exit(wcat(envvar, getenv("http_proxy")));
}

void parse_options(int argc, const char **argv, bool& bStartCleanup)
{

	bool bExtraVerb=false;
	LPCSTR szCfgDir=nullptr;
	LPCSTR szReTest=nullptr;
	bool bDumpCfg=false;
	std::vector<LPCSTR> cmdvars;

	LPCSTR PRINTCFGVAR=getenv("PRINTCFGVAR");
	if(PRINTCFGVAR)
	{
		acfg::g_bNoComplex = true;
		acfg::g_bQuiet = true;
	}

	for (auto p=argv+1; p<argv+argc; p++)
	{
		if (!strncmp(*p, "-h", 2))
			usage();
		if (!strncmp(*p, "-v", 2))
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
		else if (!strcmp(*p, "-p"))
		{
			bDumpCfg = true;
			// might need external stuff
			acfg::g_bNoComplex = false;
		}
		else if (!strcmp(*p, "--retest"))
		{
			++p;
			if(p>=argv+argc)
				exit(EXIT_FAILURE);
			szReTest = *p;
			// might need external stuff
			acfg::g_bNoComplex = false;
		}
		else if(**p) // not empty
			cmdvars.emplace_back(*p);

#if SUPPWHASH
		else if (!strncmp(*p, "-H", 2))
			exit(hashpwd());
#endif
	}

	if(szCfgDir)
		acfg::ReadConfigDirectory(szCfgDir);

	for(auto& keyval : cmdvars)
		if(!acfg::SetOption(keyval, 0))
			usage(EXIT_FAILURE);

	acfg::PostProcConfig();

	if(bExtraVerb)
		acfg::debug |= (LOG_DEBUG|LOG_MORE);

	if(bDumpCfg)
	{
		acfg::dump_config();
		exit(EXIT_SUCCESS);
	}

	if(szReTest)
	{
		LPCSTR ReTest(LPCSTR);
		std::cout << ReTest(szReTest) << std::endl;
		exit(EXIT_SUCCESS);
	}

	if(PRINTCFGVAR)
	{
		auto ps(acfg::GetStringPtr(PRINTCFGVAR));
		if(ps)
		{
			cout << *ps << endl;
			exit(EXIT_SUCCESS);
		}
		auto pi(acfg::GetIntPtr(PRINTCFGVAR));
		if(pi)
			cout << *pi << endl;
		exit(pi ? EXIT_SUCCESS : EXIT_FAILURE);
	}

}

void setup_sighandler()
{
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

	do_stuff_before_config();

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
		if (PID_FILE != NULL)
		{
			fprintf(PID_FILE, "%d", getpid());
			checkForceFclose(PID_FILE);
		}
	}
	return conserver::Run();

}

static void usage(int retCode) {
	cout <<"Usage: apt-cacher [options] [ -c configdir ] <var=value ...>\n\n"
		"Options:\n"
		"-h: this help message\n"
		"-c: configuration directory\n"
		"-e: on startup, run expiration once\n"
		"-p: print configuration and exit\n"
		"--retest string: regular expression tester"
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
	if(!Cstat(cacheDirSlash))
	{
		// well, attempt to create it then
		mstring path=cacheDirSlash+'/';
		for(uint pos=0; (pos=path.find(SZPATHSEP, pos)) < path.size(); ++pos)
			mkdir((const char*) path.substr(0,pos).c_str(), (uint) dirperms);
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
	acfg::badredmime.clear();
	acfg::redirmax=10;

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
			virtual FiStatus Setup(bool) override
			{
				m_nSizeChecked = m_nSizeSeen = 0;
				return m_status = FIST_INITED;
			}
			virtual int GetFileFd() override { return 1; }; // something, don't care for now
			virtual bool DownloadStartedStoreHeader(const header &h, const char *,
					bool, bool&) override
			{
				m_head = h;
				return true;
			}
			virtual bool StoreFileData(const char *data, unsigned int size) override
			{
				if(!size)
					m_status = FIST_COMPLETE;

				return (size==fwrite(data, sizeof(char), size, stdout));
			}
			ssize_t SendData(int , int, off_t &, size_t ) override
			{
				return 0;
			}
	};

	auto fi=std::make_shared<tPrintItem>();
	dl.AddJob(fi, &url, nullptr, nullptr);
	dl.WorkLoop();
	return (fi->WaitForFinish(NULL) == fileitem::FIST_COMPLETE
			&& fi->GetHeaderUnlocked().getStatus() == 200) ? EXIT_SUCCESS : -3;
}

