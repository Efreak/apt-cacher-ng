
#ifndef _ACFG_H
#define _ACFG_H

#include "config.h"
#include "meta.h"
#include <bitset>

#define NUM_PBKDF2_ITERATIONS 1
// 1757961
#define ACNG_DEF_PORT "3142"

namespace acng
{

struct ltstring {
	bool operator()(const mstring &s1, const mstring &s2) const {
		return strcasecmp(s1.c_str(), s2.c_str()) < 0;
	}
};

typedef std::map<mstring, mstring, ltstring> NoCaseStringMap;

namespace cfg
{
static const int RESERVED_DEFVAL = -4223;

static const int REDIRMAX_DEFAULT = 5;

extern mstring cachedir, logdir, confdir, fifopath, user, group, pidfile, suppdir,
reportpage, vfilepat, pfilepat, wfilepat, agentname, adminauth, adminauthB64,
bindaddr, port, sUmask,
tmpDontcacheReq, tmpDontcachetgt, tmpDontcache, mirrorsrcs, requestapx,
cafile, capath, spfilepat, svfilepat, badredmime, sigbuscmd, connectPermPattern;

extern mstring pfilepatEx, vfilepatEx, wfilepatEx, spfilepatEx, svfilepatEx; // for customization by user

extern int debug, numcores, offlinemode, foreground, verbose, stupidfs, forcemanaged, keepnver,
verboselog, extreshhold, exfailabort, tpstandbymax, tpthreadmax, dnscachetime, dlbufsize, usewrap,
exporigin, logxff, oldupdate, recompbz2, nettimeout, updinterval, forwardsoap, dirperms, fileperms,
maxtempdelay, redirmax, vrangeops, stucksecs, persistoutgoing, pipelinelen, exsupcount,
optproxytimeout, patrace, maxdlspeed, maxredlsize, nsafriendly, trackfileuse, exstarttradeoff;

// processed config settings
extern const tHttpUrl* GetProxyInfo();
extern void MarkProxyFailure();
extern mstring agentheader;

extern int conprotos[2];

bool SetOption(const mstring &line, NoCaseStringMap *pDupeChecker);
void dump_config(bool includingDelicateValues=false);
void ReadConfigDirectory(const char*, bool bReadErrorIsFatal=true);

//! Prepare various things resulting from variable combinations, etc.
void PostProcConfig();

bool DegradedMode();

struct tRepoData
{
	std::vector<tHttpUrl> m_backends;

	// dirty little helper to execute custom actions when a jobs associates or forgets this data set
	struct IHookHandler {
		virtual void OnAccess()=0;
		virtual void OnRelease()=0;
		virtual ~IHookHandler() {
		}
	} *m_pHooks = nullptr;
	tStrVec m_keyfiles;
	tHttpUrl m_deltasrc;
	tHttpUrl *m_pProxy = nullptr;
	virtual ~tRepoData();
};

struct tRepoResolvResult {
	cmstring* psRepoName=nullptr;
	mstring sRestPath;
	const tRepoData* repodata=nullptr;
};

/*
 * Resolves a repository descriptor for the given URL, returns a reference to its descriptor
 * (actually a pair with first: name, second: descriptor).
 *
 * @return: true IFF a repository was found and the by-reference arguments are set
 */
void GetRepNameAndPathResidual(const tHttpUrl & uri, tRepoResolvResult &result);

const tRepoData * GetRepoData(cmstring &vname);

time_t BackgroundCleanup();

extern tStrMap localdirs;
cmstring & GetMimeType(cmstring &path);
#define TCP_PORT_MAX 65536
extern std::bitset<TCP_PORT_MAX> *pUserPorts;

extern mstring cacheDirSlash; // guaranteed to have a trailing path separator

void dump_trace();
int * GetIntPtr(LPCSTR key);
mstring * GetStringPtr(LPCSTR key);

int CheckAdminAuth(LPCSTR auth);

extern bool g_bQuiet, g_bNoComplex;

static const cmstring privStoreRelSnapSufix("_xstore/rsnap");
static const cmstring privStoreRelQstatsSfx("_xstore/qstats");

} // namespace acfg

namespace rex
{

enum NOCACHE_PATTYPE : bool
{
	NOCACHE_REQ,
	NOCACHE_TGT
};

enum eMatchType : int8_t
{
	FILE_INVALID = -1,
	FILE_SOLID = 0, FILE_VOLATILE, FILE_WHITELIST,
	NASTY_PATH, PASSTHROUGH,
	FILE_SPECIAL_SOLID,
	FILE_SPECIAL_VOLATILE,
	ematchtype_max
};
bool Match(cmstring &in, eMatchType type);

eMatchType GetFiletype(const mstring &);
bool MatchUncacheable(const mstring &, NOCACHE_PATTYPE);
bool CompileUncExpressions(NOCACHE_PATTYPE type, cmstring& pat);
bool CompileExpressions();
}

#define CACHE_BASE (acng::cfg::cacheDirSlash)
#define CACHE_BASE_LEN (CACHE_BASE.length()) // where the relative paths begin
#define SZABSPATH(x) ((const char*) (CACHE_BASE+(x)).c_str())
#define SZABSPATHSFX(x, y) ((const char*) (CACHE_BASE+(x)+(y)).c_str())
#define SABSPATH(x) (CACHE_BASE+(x))

bool AppendPasswordHash(mstring &stringWithSalt, LPCSTR plainPass, size_t passLen);

// XXX: find a better place for this, shared between server and acngtool
enum ControLineType : uint8_t
{
	NotForUs = 0,
	BeforeError = 1,
	Error = 2
};
#define maark "41d_a6aeb8-26dfa" // random enough to not match anything existing *g*

}

#endif
