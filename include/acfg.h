
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

namespace cfg
{

/**
 * Configuration builder class which collects all parameters and eventually compiles the config.
 */
class ACNG_API tConfigBuilder
{
	struct tImpl;
	tImpl *m_pImpl;

public:

	tConfigBuilder(bool ignoreAllErrors, bool ignoreFileReadErrors);
	~tConfigBuilder();

	tConfigBuilder& AddOption(const mstring &line);
	tConfigBuilder& AddConfigDirectory(cmstring& sDirName);
	//! Prepare various things resulting from variable combinations, etc.
	void Build();
};

static const int RESERVED_DEFVAL = -4223;

static const int REDIRMAX_DEFAULT = 5;

extern mstring cachedir, logdir, confdir, udspath, user, group, pidfile, suppdir,
reportpage, vfilepat, pfilepat, wfilepat, agentname, adminauth, adminauthB64,
bindaddr, port, sUmask,
tmpDontcacheReq, tmpDontcachetgt, tmpDontcache, mirrorsrcs, requestapx,
cafile, capath, spfilepat, svfilepat, badredmime, sigbuscmd, connectPermPattern;

extern mstring pfilepatEx, vfilepatEx, wfilepatEx, spfilepatEx, svfilepatEx; // for customization by user

extern ACNG_API int debug, numcores, offlinemode, foreground, verbose, stupidfs, forcemanaged, keepnver,
verboselog, extreshhold, exfailabort, tpstandbymax, tpthreadmax, dnscachetime, dlbufsize, usewrap,
exporigin, logxff, oldupdate, recompbz2, nettimeout, updinterval, forwardsoap, dirperms, fileperms,
maxtempdelay, redirmax, vrangeops, stucksecs, persistoutgoing, pipelinelen, exsupcount,
optproxytimeout, patrace, maxdlspeed, maxredlsize, dlretriesmax, nsafriendly, trackfileuse, exstarttradeoff,
fasttimeout, discotimeout;
extern int allocspace;

// processed config settings
extern const tHttpUrl* GetProxyInfo();
extern void MarkProxyFailure();
extern mstring agentheader;

extern int conprotos[2];


void ACNG_API dump_config(bool includingDelicateValues=false);

bool DegradedMode();

struct IHookHandler {
		virtual void OnAccess()=0;
		virtual void OnRelease()=0;
		virtual ~IHookHandler() {}
};
struct tRepoData
{
	std::vector<tHttpUrl> m_backends;

	// dirty little helper to execute custom actions when a jobs associates or forgets this data set
	IHookHandler *m_pHooks = nullptr;
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
ACNG_API int * GetIntPtr(string_view key);
ACNG_API mstring * GetStringPtr(string_view key);

int CheckAdminAuth(LPCSTR auth);

static const cmstring privStoreRelSnapSufix("_xstore/rsnap");
static const cmstring privStoreRelQstatsSfx("_xstore/qstats");

} // namespace acfg

#define CACHE_BASE (acng::cfg::cacheDirSlash)
#define CACHE_BASE_LEN (CACHE_BASE.length()) // where the relative paths begin
#define SZABSPATH(x) (CACHE_BASE+(x)).c_str()
#define SABSPATH(x) (CACHE_BASE+(x))

#define SABSPATHEX(x, y) (CACHE_BASE+(x) + (y))
#define SZABSPATHEX(x, y) (CACHE_BASE+(x) + (y)).c_str()

// XXX: find a better place for this, shared between server and acngtool
enum ControLineType : uint8_t
{
	NotForUs = 0,
	BeforeError = 1,
	Error = 2
};
#define maark "41d_a6aeb8-26dfa" // random enough to not match anything existing *g*

inline static mstring DetoxPath4Cache(cmstring &sPathRaw)
{
	return cfg::stupidfs ? DosEscape(sPathRaw) : sPathRaw;
}

}

#endif
