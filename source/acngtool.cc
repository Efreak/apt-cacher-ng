#include "config.h"
#include "meta.h"
#include "acfg.h"

#include <acbuf.h>
#include <aclogger.h>
#include <fcntl.h>

#ifdef HAVE_SSL
#include <openssl/evp.h>
#endif

#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <signal.h>
#include <regex.h>
#include <errno.h>

#include <cstdbool>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <functional>

#include <iostream>
#include <fstream>
#include <string>
#include <list>

#include "debug.h"
#include "dlcon.h"
#include "fileio.h"
#include "fileitem.h"

using namespace std;

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
#include "cleaner.h"

bool g_bVerbose = false;

// dummies to satisfy references to cleaner callbacks
cleaner::cleaner() : m_thr(pthread_t()) {}
cleaner::~cleaner() {}
void cleaner::ScheduleFor(time_t, cleaner::eType) {}
cleaner g_victor;

struct IFitemFactory
{
	virtual SHARED_PTR<fileitem> Create() =0;
	virtual ~IFitemFactory() =default;
};

struct CPrintItemFactory : public IFitemFactory
{
	virtual SHARED_PTR<fileitem> Create()
	{
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
			virtual int GetFileFd() override
			{	return 1;}; // something, don't care for now
			virtual bool DownloadStartedStoreHeader(const header &h, size_t, const char *,
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
		return make_shared<tPrintItem>();
	}
};

struct verbprint
{
	int cnt = 0;
	void dot()
	{
		if (!g_bVerbose)
			return;
		cnt++;
		cerr << '.';
	}
	void msg(cmstring& msg)
	{
		if (!g_bVerbose)
			return;
		fin();
		cerr << msg << endl;

	}
	void fin()
	{
		if (!g_bVerbose)
			return;
		if (cnt)
			cerr << endl;
		cnt = 0;
	}
} vprint;

struct CReportItemFactory : public IFitemFactory
{
	virtual SHARED_PTR<fileitem> Create()
	{
		class tRepItem : public fileitem
		{
			acbuf lineBuf;
			string m_key = maark;
			tStrVec m_errMsg;

		public:

			tRepItem()
			{
				m_bAllowStoreData=false;
				m_nSizeChecked = m_nSizeSeen = 0;
				lineBuf.setsize(1<<16);
				memset(lineBuf.wptr(), 0, 1<<16);
			};
			virtual FiStatus Setup(bool) override
			{
				m_nSizeChecked = m_nSizeSeen = 0;
				return m_status = FIST_INITED;
			}
			virtual int GetFileFd() override
			{	return 1;}; // something, don't care for now
			virtual bool DownloadStartedStoreHeader(const header &h, size_t, const char *,
					bool, bool&) override
			{
				m_head = h;
				return true;
			}
			virtual bool StoreFileData(const char *data, unsigned int size) override
			{
				if(!size)
				{
					m_status = FIST_COMPLETE;
					vprint.fin();
				}
				auto consumed = std::min(size, lineBuf.freecapa());
				memcpy(lineBuf.wptr(), data, consumed);
				lineBuf.got(consumed);
				for(;;)
				{
					LPCSTR p = lineBuf.rptr();
					auto end = mempbrk(p, "\r\n", lineBuf.size());
					if(!end)
						break;
					string s(p, end-p);
					lineBuf.drop(s.length()+1);
					vprint.dot();
					if(startsWith(s, m_key))
					{
						// that's for us... "<key><type> content\n"
						char *endchar = nullptr;
						p = s.c_str();
						auto val = strtoul(p + m_key.length(), &endchar, 10);
						if(!endchar || !*endchar)
							continue; // heh? shall not finish here
						switch(ControLineType(val))
						{
						case ControLineType::BeforeError:
							m_errMsg.emplace_back(endchar, s.size() - (endchar - p));
							vprint.msg(m_errMsg.back());
							break;
						case ControLineType::Error:
						{
							if(!g_bVerbose) // printed before
								for(auto l : m_errMsg)
									cerr << l << endl;
							m_errMsg.clear();
							string msg(endchar, s.size() - (endchar - p));
							vprint.fin();
							cerr << msg << endl;
							break;
						}
						default:
							continue;
						}
					}
				}
				return true;
			}
			ssize_t SendData(int , int, off_t &, size_t ) override
			{
				return 0;
			}
		};
		return make_shared<tRepItem>();
	}
};

int wcat(LPCSTR url, LPCSTR proxy, IFitemFactory*, IDlConFactory *pdlconfa = &g_tcp_con_factory);

LPCSTR ReTest(LPCSTR);

static void usage(int retCode = 0)
{
	(retCode ? cout : cerr) <<
		"Usage: acngtool command parameter... [options]\n\n"
			"command := { printvar, cfgdump, retest, patch, curl, encb64, maint }\n"
			"parameter := (specific to command)\n"
			"options := (see apt-cacher-ng options)\n"
			"extra options := -h, --verbose\n"
#if SUPPWHASH
#warning FIXME
			"-H: read a password from STDIN and print its hash\n"
#endif
"\n";
			exit(retCode);
}


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

typedef deque<tPtrLen> tPatchSequence;

// might need to access the last line externally
unsigned long rangeStart(0), rangeLast(0);

inline bool patchChunk(tPatchSequence& idx, LPCSTR pline, size_t len, tPatchSequence chunk)
{
	char op = 0x0;
	auto n = sscanf(pline, "%lu,%lu%c\n", &rangeStart, &rangeLast, &op);
	if (n == 1) // good enough
		rangeLast = rangeStart, op = pline[len - 2];
	else if(n!=3)
		return false; // bad instruction
	if (rangeStart > idx.size() || rangeLast > idx.size() || rangeStart > rangeLast)
		return false;
	if (op == 'a')
		idx.insert(idx.begin() + (size_t) rangeStart + 1, chunk.begin(), chunk.end());
	else
	{
		size_t i = 0;
		for (; i < chunk.size(); ++i, ++rangeStart)
		{
			if (rangeStart <= rangeLast)
				idx[rangeStart] = chunk[i];
			else
				break; // new stuff bigger than replaced range
		}
		if (i < chunk.size()) // not enough space :-(
			idx.insert(idx.begin() + (size_t) rangeStart, chunk.begin() + i, chunk.end());
		else if (rangeStart - 1 != rangeLast) // less data now?
			idx.erase(idx.begin() + (size_t) rangeStart, idx.begin() + (size_t) rangeLast + 1);
	}
	return true;
}

int maint_job()
{
   acfg::SetOption("proxy=", nullptr);
	LPCSTR envh = getenv("HOSTNAME");
	if (!envh)
		envh = "localhost";
	LPCSTR req = getenv("ACNGREQ");
	if (!req)
		req = "?doExpire=Start+Expiration&abortOnErrors=aOe";

	tSS urlPath;
	urlPath << "http://";
	if(!acfg::adminauth.empty())
		urlPath << acfg::adminauth << "@";
	urlPath << envh << ":" << acfg::port;

	if(acfg::reportpage.empty())
		return -1;
	if(acfg::reportpage[0] != '/')
		urlPath << '/';
	urlPath << acfg::reportpage << req;

	CReportItemFactory fac;

	int s = -1;
	if (!acfg::fifopath.empty())
	{
#ifdef DEBUG
		cerr << "Socket path: " << acfg::fifopath << endl;
#endif
		s = socket(PF_UNIX, SOCK_STREAM, 0);
		if (s >= 0)
		{
			struct sockaddr_un addr;
			addr.sun_family = PF_UNIX;
			strcpy(addr.sun_path, acfg::fifopath.c_str());
			socklen_t adlen = acfg::fifopath.length() + 1 + offsetof(struct sockaddr_un, sun_path);
			if (0 != connect(s, (struct sockaddr*) &addr, adlen))
			{
				s = -1;
#ifdef DEBUG
				perror("connect");
#endif
			}
			else
			{
				//identify myself
				tSS ids;
				ids << "GET / HTTP/1.0\r\nX-Original-Source: localhost\r\n\r\n";
				if (!ids.send(s))
					s = -1;
			}
		}
	}

	if(s>=0) // use hot unix socket
	{
		struct udsFac: public IDlConFactory
		{
			int m_sockfd = -1;
			udsFac(int n) : m_sockfd(n) {}

			void RecycleIdleConnection(tDlStreamHandle & handle) override
			{}
			virtual tDlStreamHandle CreateConnected(cmstring &, cmstring &, mstring &, bool *,
					acfg::tRepoData::IHookHandler *, bool, int, bool) override
			{
				struct udsconnection : public tcpconnect
				{
					udsconnection(int s) : tcpconnect(nullptr)
					{
						m_conFd = s;
#ifdef HAVE_SSL
						m_ssl = nullptr;
						m_bio = nullptr;
#endif
						// must match the URL parameters
						m_sHostName = "localhost";
						m_sPort = acfg::port;

					}
				};
				return make_shared<udsconnection>(m_sockfd);
			}
		} udsfac(s);
		wcat(urlPath.c_str(), nullptr, &fac, &udsfac);
	}
	else // ok, try the TCP path
	{
		wcat(urlPath.c_str(), getenv("http_proxy"), &fac);
	}
	return 0;
}

int patch_file(string sBase, string sPatch, string sResult)
{
	filereader frBase, frPatch;
	if(!frBase.OpenFile(sBase, true) || !frPatch.OpenFile(sPatch, true))
		return -2;
	auto buf = frBase.GetBuffer();
	auto size = frBase.GetSize();
	tPatchSequence idx;
	idx.emplace_back(buf, 0); // dummy entry to avoid -1 calculations because of ed numbering style
	for (auto p = buf; p < buf + size;)
	{
		LPCSTR crNext = strchr(p, '\n');
		if (crNext)
		{
			idx.emplace_back(p, crNext + 1 - p);
			p = crNext + 1;
		}
		else
		{
			idx.emplace_back(p, buf + size - p);
			break;
		}
	}

	auto pbuf = frPatch.GetBuffer();
	auto psize = frPatch.GetSize();
	tPatchSequence chunk;
	LPCSTR cmd =0;
	size_t cmdlen = 0;
	for (auto p = pbuf; p < pbuf + psize;)
	{
		LPCSTR crNext = strchr(p, '\n');
		size_t len = 0;
		LPCSTR line=p;
		if (crNext)
		{
			len = crNext + 1 - p;
			p = crNext + 1;
		}
		else
		{
			len = pbuf + psize - p;
			p = pbuf + psize + 1; // break signal, actually
		}
		p=crNext+1;

		bool gogo = (len == 2 && *line == '.');
		if(!gogo)
		{
			if(!cmdlen)
			{
				if(!strncmp("s/.//\n", line, 6))
				{
					// oh, that's the fix-the-last-line command :-(
					if(rangeStart)
						idx[rangeStart].first = ".\n", idx[rangeStart].second=2;
					continue;
				}
				else if(line[0] == 'w')
					continue; // don't care, we know the target

				cmdlen = len;
				cmd = line;

				if(len>2 && line[len-2] == 'd')
					gogo = true; // no terminator to expect
			}
			else
				chunk.emplace_back(line, len);
		}

		if(gogo)
		{
			if(!patchChunk(idx, cmd, cmdlen, chunk))
			{
				cerr << "Bad patch line: ";
				cerr.write(cmd, cmdlen);
				exit(EINVAL);
			}
			chunk.clear();
			cmdlen = 0;
		}
	}
	ofstream res(sResult.c_str());
	if(!res.is_open())
		return -3;

	for(const auto& kv : idx)
		res.write(kv.first, kv.second);
	res.flush();
//	dump_proc_status_always();
	return res.good() ? 0 : -4;
}


struct parm {
	unsigned minArg, maxArg;
	std::function<void(LPCSTR)> f;
};

// some globals shared across the functions
int g_exitCode(0);
LPCSTR g_missingCfgDir = nullptr;

void parse_options(int argc, const char **argv, function<void (LPCSTR)> f)
{
	LPCSTR szCfgDir=CFGDIR;
	std::vector<LPCSTR> validargs, nonoptions;

	for (auto p=argv; p<argv+argc; p++)
	{
		if (!strncmp(*p, "-h", 2))
			usage();
		else if (!strcmp(*p, "-c"))
		{
			++p;
			if (p < argv + argc)
				szCfgDir = *p;
			else
				usage(2);
		}
		else if(!strcmp(*p, "--verbose"))
			g_bVerbose=true;
		else if(**p) // not empty
			validargs.emplace_back(*p);

#if SUPPWHASH
#warning FIXME
		else if (!strncmp(*p, "-H", 2))
			exit(hashpwd());
#endif
	}

	if(szCfgDir)
	{
		Cstat info(szCfgDir);
		if(!info || !S_ISDIR(info.st_mode))
			g_missingCfgDir = szCfgDir;
		else
			acfg::ReadConfigDirectory(szCfgDir, false);
	}

	tStrVec non_opt_args;

	for(auto& keyval : validargs)
		if(!acfg::SetOption(keyval, 0))
			nonoptions.emplace_back(keyval);

	acfg::PostProcConfig();

	for(const auto& x: nonoptions)
		f(x);
}


#if SUPPWHASH
void ssl_init()
{
#ifdef HAVE_SSL
	SSL_load_error_strings();
	ERR_load_BIO_strings();
	ERR_load_crypto_strings();
	ERR_load_SSL_strings();
	OpenSSL_add_all_algorithms();
	SSL_library_init();
#endif
}
#endif

/*
void assert_cfgdir()
{
	if(!g_missingCfgDir)
		return;
	cerr << "Failed to open config directory: " << g_missingCfgDir <<endl;
	exit(EXIT_FAILURE);
}
*/

void warn_cfgdir()
{
	if (g_missingCfgDir)
		cerr << "Warning: failed to open config directory: " << g_missingCfgDir <<endl;
}

std::unordered_map<string, parm> parms = {
#if 0
   {
		"urltest",
		{ 1, 1, [](LPCSTR p)
			{
				std::cout << EncodeBase64Auth(p);
			}
		}
	}
	,
#endif
	{
		"encb64",
		{ 1, 1, [](LPCSTR p)
			{
#ifdef DEBUG
			cerr << "encoding " << p <<endl;
#endif
				std::cout << EncodeBase64Auth(p);
			}
		}
	}
	,
		{
			"cfgdump",
			{ 0, 0, [](LPCSTR p) {
				warn_cfgdir();
						     acfg::dump_config(false);
					     }
			}
		}
	,
		{
			"curl",
			{ 1, UINT_MAX, [](LPCSTR p)
				{
					CPrintItemFactory fac;
					auto ret=wcat(p, getenv("http_proxy"), &fac);
					if(!g_exitCode)
						g_exitCode = ret;

				}
			}
		},
		{
			"retest",
			{
				1, 1, [](LPCSTR p)
				{
					warn_cfgdir();
					std::cout << ReTest(p) << std::endl;
				}
			}
		}
	,
		{
			"printvar",
			{
				1, 1, [](LPCSTR p)
				{
					warn_cfgdir();
					auto ps(acfg::GetStringPtr(p));
					if(ps) { cout << *ps << endl; return; }
					auto pi(acfg::GetIntPtr(p));
					if(pi) {
						cout << *pi << endl;
						return;
					}
					g_exitCode=23;
				}
			}
		},
		{ 
			"patch",
			{
				3, 3, [](LPCSTR p)
				{
					static tStrVec iop;
					iop.emplace_back(p);
					if(iop.size() == 3)
						g_exitCode+=patch_file(iop[0], iop[1], iop[2]);
				}
			}
		}

	,
		{
			"maint",
			{
				0, 0, [](LPCSTR p)
				{
					warn_cfgdir();
					g_exitCode+=maint_job();
				}
			}
		}
};

int main(int argc, const char **argv)
{
	string exe(argv[0]);
	unsigned aOffset=1;
	if(endsWithSzAr(exe, "expire-caller.pl"))
	{
		aOffset=0;
		argv[0] = "maint";
	}
	acfg::g_bQuiet = true;
	acfg::g_bNoComplex = true; // no DB for just single variables

  parm* parm = nullptr;
  LPCSTR mode = nullptr;
  unsigned xargCount = 0;

	parse_options(argc-aOffset, argv+aOffset, [&](LPCSTR p)
			{
		bool bFirst = false;
		if(!mode)
			bFirst = (0 != (mode = p));
		else
			xargCount++;
		if(!parm)
			{
			auto it = parms.find(mode);
			if(it == parms.end())
				usage(1);
			parm = & it->second;
			}
		if(xargCount > parm->maxArg)
			usage(2);
		if(!bFirst)
			parm->f(p);
			});
	if(!mode || !parm)
		usage(3);
	if(!xargCount) // should run the code at least once?
	{
		if(parm->minArg) // uh... needs argument(s)
			usage(4);
		parm->f(nullptr);
	}
	return g_exitCode;
}

int wcat(LPCSTR surl, LPCSTR proxy, IFitemFactory* fac, IDlConFactory *pDlconFac)
{
	acfg::dnscachetime=0;
	acfg::persistoutgoing=0;
	acfg::badredmime.clear();
	acfg::redirmax=10;

	if(proxy)
		if(acfg::SetOption(string("proxy:")+proxy, nullptr))
			return -1;
	tHttpUrl url;
	if(!surl)
		return 2;
	string xurl(surl);
	if(!url.SetHttpUrl(xurl))
		return -2;
	dlcon dl(true, nullptr, pDlconFac);

	auto fi=fac->Create();
	dl.AddJob(fi, &url, nullptr, nullptr, 0);
	dl.WorkLoop();
	if(fi->GetStatus() == fileitem::FIST_COMPLETE)
	{
		auto hh=fi->GetHeaderUnlocked();
		auto st=hh.getStatus();
		if(st == 200)
			return EXIT_SUCCESS;
		// don't reveal passwords
		auto xpos=xurl.find('@');
		if(xpos!=stmiss)
			xurl.erase(0, xpos+1);
		cerr << "Error: cannot fetch " << xurl <<", "  << hh.frontLine << endl;
		if (st>=500)
			return EIO;
		if (st>=400)
			return EACCES;
	}

	return EXIT_FAILURE;
}

#if 0

void do_stuff_before_config()
{
	LPCSTR envvar(nullptr);

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
}

#endif
#if 0
#warning line reader test enabled
	if (cmd == "wcl")
	{
		if (argc < 3)
			usage(2);
		filereader r;
		if (!r.OpenFile(argv[2], true))
		{
			cerr << r.getSErrorString() << endl;
			return EXIT_FAILURE;
		}
		size_t count = 0;
		auto p = r.GetBuffer();
		auto e = p + r.GetSize();
		for (;p < e; ++p)
			count += (*p == '\n');
		cout << count << endl;

		exit(EXIT_SUCCESS);
	}
#endif
#if 0
#warning header parser enabled
	if (cmd == "htest")
	{
		header h;
		h.LoadFromFile(argv[2]);
		cout << string(h.ToString()) << endl;

		h.clear();
		filereader r;
		r.OpenFile(argv[2]);
		std::vector<std::pair<std::string, std::string>> oh;
		h.Load(r.GetBuffer(), r.GetSize(), &oh);
		for(auto& r : oh)
			cout << "X:" << r.first << " to " << r.second;
		exit(0);
	}
#endif
#if 0
#warning benchmark enabled
	if (cmd == "benchmark")
	{
		dump_proc_status_always();
		acfg::g_bQuiet = true;
		acfg::g_bNoComplex = false;
		parse_options(argc - 2, argv + 2, true);
		acfg::PostProcConfig();
		string s;
		tHttpUrl u;
		int res=0;
/*
		acfg::tRepoResolvResult hm;
		tHttpUrl wtf;
		wtf.SetHttpUrl(non_opt_args.front());
		acfg::GetRepNameAndPathResidual(wtf, hm);
*/
		while(cin)
		{
			std::getline(cin, s);
			s += "/xtest.deb";
			if(u.SetHttpUrl(s))
			{
				acfg::tRepoResolvResult xdata;
				acfg::GetRepNameAndPathResidual(u, xdata);
				cout << s << " -> "
						<< (xdata.psRepoName ? "matched" : "not matched")
						<< endl;
			}
		}
		dump_proc_status_always();
		exit(res);
	}
#endif
