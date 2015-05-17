#include "config.h"
#include "meta.h"
#include "acfg.h"

#include <acbuf.h>
#include <aclogger.h>
#include <fcntl.h>
#include <openssl/evp.h>
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


int wcat(LPCSTR url, LPCSTR proxy);
LPCSTR ReTest(LPCSTR);


// dummies to satisfy references to cleaner callbacks
cleaner::cleaner() : m_thr(pthread_t()) {}
cleaner::~cleaner() {}
void cleaner::ScheduleFor(time_t, cleaner::eType) {}
cleaner g_victor;



static void usage(int retCode = 0)
{
	(retCode ? cout : cerr) <<
		"Usage: acngtool command parameter... [options]\n\n"
			"command := { printvar, cfgdump, retest, patch, curl, encb64 }\n"
			"parameter := (specific to command)\n"
			"options := (see apt-cacher-ng options)\n"
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

int patch_file(string sBase, string sPatch, string sResult)
{
	filereader frBase, frPatch;
	if(!frBase.OpenFile(sBase, true) || !frPatch.OpenFile(sPatch, true))
		return -2;
	typedef pair<LPCSTR, size_t> tPtrLen;
	list<tPtrLen> idx;
	auto buf = frBase.GetBuffer();
	auto size = frBase.GetSize();
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
#if 0
	int i=10;
	for(auto& kv : idx)
	{
		if(i--<0) break;
		cerr << "O:" << string(kv.first, kv.second);
	}
#endif
	// start at the fake position after EOF
	unsigned long cursor = idx.size()+1; // one-shifted like in ed
	auto iter = idx.end();
	auto patchChunk = [&](list<tPtrLen> chunk)
			{
		if(chunk.empty())
			return false;
		auto pline = chunk.front().first;
		auto len = chunk.front().second;
		unsigned long rangeStart, rangeEnd;
		char op = 0x0;
		auto n = sscanf(pline, "%lu,%lu%c\n", &rangeStart, &rangeEnd, &op);
		//cerr << n <<endl;
		if(n == 1) // good enough
			rangeEnd = rangeStart, op=pline[len-2];
		if(rangeStart>idx.size() || rangeEnd>idx.size() || rangeStart>rangeEnd)
			return false;
		if(rangeStart > cursor)
		{
			cursor = idx.size()+1;
			iter = idx.end();
		}
		while(rangeStart < cursor) // good enough since diff delivers patches in reverse order
		{
			iter--;
			cursor--;
		}
		// delete old stuff that will be replaced
		// ok, cursor remains at range start
		if(op != 'a')
		{
			while(rangeEnd >= rangeStart)
				iter = chunk.erase(iter), rangeEnd--;
		}
		else // append AFTER that line
			iter++, cursor++;

		auto beginNew = chunk.begin();
		beginNew++;
		iter = idx.insert(iter, beginNew, chunk.end());

		return true;

			};

	auto pbuf = frPatch.GetBuffer();
	auto psize = frPatch.GetSize();
	list<tPtrLen> chunk;
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
		// ok, got the line as line with len
		//cerr << "patch line: " << string(line, len);
		//cerr.flush();
		bool gogo = (len == 2 && *line == '.');
		if(!gogo)
			chunk.emplace_back(line, len);
		if(chunk.size() == 1 && line[len-2] == 'd')
			gogo = true; // no terminator
		if(gogo)
		{
			if(!patchChunk(chunk))
				exit(-5);
			chunk.clear();
		}
	}
	ofstream res(sResult.c_str());
	if(!res.is_open())
		return -3;

	for(auto& kv : idx)
		res.write(kv.first, kv.second);
	res.flush();
	return res.good() ? 0 : -4;
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

void parse_options(int argc, const char **argv)
{
	LPCSTR szCfgDir=CFGDIR;
	std::vector<LPCSTR> cmdvars;

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
		else if(**p) // not empty
			cmdvars.emplace_back(*p);

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
		{
			cerr << "Failed to open config directory: " << szCfgDir;
			exit(-2);
		}
		acfg::ReadConfigDirectory(szCfgDir, false);
	}

	for(auto& keyval : cmdvars)
		if(!acfg::SetOption(keyval, 0))
			usage(EXIT_FAILURE);

	acfg::PostProcConfig();
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

int main(int argc, const char **argv)
{
	if(argc<2)
		usage(1);
	string cmd(argv[1]);
	if(cmd == "encb64")
	{
		if(argc<3)
			usage(2);
		std::cout << EncodeBase64Auth(argv[2]);
		exit(EXIT_SUCCESS);
	}
	if(cmd == "curl")
	{
		if(argc<3)
			usage(2);
		else
			exit(wcat(argv[2], getenv("http_proxy")));
	}
	if(cmd == "printvar" || cmd == "retest" || cmd == "cfgdump")
	{
		acfg::g_bQuiet = true;
		acfg::g_bNoComplex = (cmd[0] != 'p'); // no DB for just single variables
		parse_options(argc-3, argv+3);
		switch(cmd[0])
		{
		case 'c':
			acfg::dump_config();
			exit(EXIT_SUCCESS);
		break;
		case 'r':
			if(argc<3)
				usage(2);
			std::cout << ReTest(argv[2]) << std::endl;
			exit(EXIT_SUCCESS);
		case 'p':
			if(argc<3)
				usage(2);
			auto ps(acfg::GetStringPtr(argv[2]));
			if(ps)
			{
				cout << *ps << endl;
				exit(EXIT_SUCCESS);
			}
			auto pi(acfg::GetIntPtr(argv[2]));
			if(pi)
				cout << *pi << endl;
			exit(pi ? EXIT_SUCCESS : EXIT_FAILURE);
		}
	}
	if(cmd == "patch")
	{
		if(argc!=5)
			usage(3);
		return(patch_file(argv[2], argv[3], argv[4]));
				/*
			"dev/diff/Packages.orig",
			"dev/diff/test.diff",
			"dev/diff/patched"
	*/
	}

	usage(4);
	return -1;
}

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

