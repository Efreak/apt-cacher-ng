
#ifndef _ACFG_H
#define _ACFG_H

#include "config.h"
#include "meta.h"
#include <bitset>

static const int RESERVED_DEFVAL = -4223;
#define ACNG_DEF_PORT "3142"

#define ACFG_REDIRMAX_DEFAULT 5

struct ltstring
{
  bool operator()(const mstring &s1, const mstring &s2) const
  {
    return strcasecmp(s1.c_str(), s2.c_str()) < 0;
  }
};

typedef std::map<mstring, mstring, ltstring> NoCaseStringMap;

namespace acfg
{

extern mstring cachedir, logdir, confdir, fifopath, user, group, pidfile, suppdir,
reportpage, vfilepat, pfilepat, wfilepat, agentname, adminauth, bindaddr, port, sUmask,
tmpDontcacheReq, tmpDontcachetgt, tmpDontcache, mirrorsrcs, requestapx,
cafile, capath;

extern mstring pfilepatEx, vfilepatEx, wfilepatEx; // for customization by user

extern int debug, numcores, offlinemode, foreground, verbose, stupidfs, forcemanaged, keepnver,
verboselog, extreshhold, exfailabort, tpstandbymax, tpthreadmax, dnscachetime, dlbufsize, usewrap,
exporigin, logxff, oldupdate, recompbz2, nettimeout, updinterval, forwardsoap, dirperms, fileperms,
maxtempdelay, redirmax, vrangeops, stucksecs, persistoutgoing, pipelinelen, exsupcount,
optproxytimeout, patrace;

// processed config settings
extern tHttpUrl proxy_info;
extern mstring agentheader;

extern int conprotos[2];

bool SetOption(const mstring &line, bool bQuiet=false, NoCaseStringMap *pDupeChecker=NULL);
void ReadConfigDirectory(const char*, bool bTestMode);

//! Prepare various things resulting from variable combinations, etc.
void PostProcConfig(bool bDumpConfig);

// TODO: document me
// throw away the rewritten part of the path, foo/debian/bla.deb -> bla.deb,
// no slash needed with backends...

struct tRepoData
{
	std::vector<tHttpUrl> m_backends;

	// dirty little helper to execute custom actions when a jobs associates or forgets this data set
	struct IHookHandler
	{
		virtual void JobRelease()=0;
		virtual void JobConnect()=0;
    virtual ~IHookHandler() {};
	};
	IHookHandler *m_pHooks;
	tStrVec m_keyfiles;
	tHttpUrl m_deltasrc;
	tHttpUrl *m_pProxy;
	tRepoData() : m_pHooks(NULL), m_pProxy(NULL) {};
	virtual ~tRepoData();
};

typedef std::map<cmstring, tRepoData>::const_iterator tBackendDataRef;

bool GetRepNameAndPathResidual(const tHttpUrl & in, mstring & sRetPathResidual, tBackendDataRef &beRef);

const tRepoData * GetBackendVec(cmstring &vname);

time_t BackgroundCleanup();

extern tStrMap localdirs;
cmstring & GetMimeType(cmstring &path);
#define TCP_PORT_MAX 65536
extern std::bitset<TCP_PORT_MAX> *pUserPorts;

extern mstring cacheDirSlash; // guaranteed to have a trailing path separator

bool appendVar(LPCSTR varname, mstring& ret);
void dump_trace();
int * GetIntPtr(LPCSTR key, int &base);
} // namespace acfg

namespace rechecks
{

enum NOCACHE_PATTYPE : bool
{
	NOCACHE_REQ,
	NOCACHE_TGT
};

enum eMatchType
{
	FILE_INVALID = -1,
	FILE_SOLID = 0, FILE_VOLATILE, FILE_WHITELIST,
	NASTY_PATH, PASSTHROUGH,
	ematchtype_max
};
uint_fast8_t Match(cmstring &in, eMatchType type);

eMatchType GetFiletype(const mstring &);
bool MatchUncacheable(const mstring &, NOCACHE_PATTYPE);
bool CompileUncExpressions(NOCACHE_PATTYPE type, cmstring& pat);
bool CompileExpressions();
}

#define CACHE_BASE (acfg::cacheDirSlash)
#define CACHE_BASE_LEN (CACHE_BASE.length()) // where the relative paths begin
#define SZABSPATH(x) (CACHE_BASE+(x)).c_str()
#define SABSPATH(x) (CACHE_BASE+(x))

#endif
