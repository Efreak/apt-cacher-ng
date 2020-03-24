
#include "debug.h"

#include "acfg.h"
#include "meta.h"
#include "filereader.h"
#include "fileio.h"
#include "sockio.h"
#include "lockable.h"
#include "cleaner.h"
#include "csmapping.h"
#include "rex.h"
#include "dbman.h"

#include <iostream>
#include <fstream>
#include <deque>
#include <algorithm>
#include <list>
#include <unordered_map>
#include <atomic>

using namespace std;

namespace acng
{
// hint to use the main configuration excluding the complex directives
//bool g_testMode=false;

bool bIsHashedPwd=false;


struct ltstring {
	bool operator()(const mstring &s1, const mstring &s2) const {
		return strcasecmp(s1.c_str(), s2.c_str()) < 0;
	}
};

typedef std::map<mstring, mstring, ltstring> NoCaseStringMap;
typedef std::map<mstring, ltstring> NoCaseStringSet;

namespace cfg {

extern std::atomic_bool degraded;

// internal stuff:
string sPopularPath("/debian/");
string tmpDontcache, tmpDontcacheReq, tmpDontcacheTgt, optProxyCheckCmd;
int optProxyCheckInt = 99;

tStrMap localdirs;
static class : public base_with_mutex, public NoCaseStringMap {} mimemap;

std::bitset<TCP_PORT_MAX> *pUserPorts = nullptr;

tHttpUrl proxy_info;

void _ParseLocalDirs(string_view value)
{
	for(auto sliceFromTo: tSplitWalk(value, ";", true))
	{
		trimBoth(sliceFromTo);
		tStrPos pos = sliceFromTo.find_first_of(SPACECHARS);
		if(stmiss == pos)
		{
			cerr << "Cannot map " << sliceFromTo << ", needed format: virtualdir realdir, ignoring it";
			continue;
		}
		// XXX: this is still kinda PITA, the best format which works here is: /foo "/var/cache/fooish bar/sdf"
		string_view from(sliceFromTo.data(), pos);
		trimBoth(from, "/");
		sliceFromTo.remove_prefix(pos);
		trimBoth(sliceFromTo, SPACECHARS "'\"");
		if(sliceFromTo.empty())
		{
			cerr << "Unsupported target of " << from << ": " << sliceFromTo << ", ignoring it" << endl;
			continue;
		}
		localdirs[to_string(from)]=to_string(sliceFromTo);
	}
}

struct MapNameToString
{
	string_view name; mstring *ptr;
};

struct MapNameToInt
{
	string_view name; int *ptr;
	const char *warn; uint8_t base;
	uint8_t hidden;	// just a hint
};

struct tProperty
{
	string_view name;
	std::function<bool(string_view key, string_view value)> set;
	std::function<mstring(bool superUser)> get; // returns a string value. A string starting with # tells to skip the output
};

map<cmstring, tRepoData> repoparms;
typedef decltype(repoparms)::iterator tPairRepoNameData;
// maps hostname:port -> { <pathprefix,repopointer>, ... }
std::unordered_map<string, list<pair<cmstring,tPairRepoNameData>>> mapUrl2pVname;

MapNameToString n2sTbl[] = {
		{   "Port",                    &port}
		,{  "CacheDir",                &cachedir}
		,{  "LogDir",                  &logdir}
		,{  "SupportDir",              &suppdir}
		,{  "SocketPath",              &udspath}
		,{  "PidFile",                 &pidfile}
		,{  "ReportPage",              &reportpage}
		,{  "VfilePattern",            &vfilepat}
		,{  "PfilePattern",            &pfilepat}
		,{  "SPfilePattern",           &spfilepat}
		,{  "SVfilePattern",           &svfilepat}
		,{  "WfilePattern",            &wfilepat}
		,{  "VfilePatternEx",          &vfilepatEx}
		,{  "PfilePatternEx",          &pfilepatEx}
		,{  "WfilePatternEx",          &wfilepatEx}
		,{  "SPfilePatternEx",         &spfilepatEx}
		,{  "SVfilePatternEx",         &svfilepatEx}
//		,{  "AdminAuth",               &adminauth}
		,{  "BindAddress",             &bindaddr}
		,{  "UserAgent",               &agentname}
		,{  "DontCache",               &tmpDontcache}
		,{  "DontCacheRequested",      &tmpDontcacheReq}
		,{  "DontCacheResolved",       &tmpDontcacheTgt}
		,{  "PrecacheFor",             &mirrorsrcs}
		,{  "RequestAppendix",         &requestapx}
		,{  "PassThroughPattern",      &connectPermPattern}
		,{  "CApath",                  &capath}
		,{  "CAfile",                  &cafile}
		,{  "BadRedirDetectMime",      &badredmime}
		,{	"OptProxyCheckCommand",	   &optProxyCheckCmd}
		,{  "BusAction",                &sigbuscmd} // "Special debugging helper, see manual!"
};

MapNameToInt n2iTbl[] = {
		{   "Debug",                             &debug,            nullptr,    10, false}
		,{  "OfflineMode",                       &offlinemode,      nullptr,    10, false}
		,{  "ForeGround",                        &foreground,       nullptr,    10, false}
		,{  "ForceManaged",                      &forcemanaged,     nullptr,    10, false}
		,{  "StupidFs",                          &stupidfs,         nullptr,    10, false}
		,{  "VerboseLog",                        &verboselog,       nullptr,    10, false}
		,{  "ExThreshold",                       &extreshhold,      nullptr,    10, false}
		,{  "ExTreshold",                        &extreshhold,      nullptr,    10, true} // wrong spelling :-(
		,{  "MaxStandbyConThreads",              &tpstandbymax,     nullptr,    10, false}
		,{  "MaxConThreads",                     &tpthreadmax,      nullptr,    10, false}
		,{  "DlMaxRetries",                      &dlretriesmax,     nullptr,    10, false}
		,{  "DnsCacheSeconds",                   &dnscachetime,     nullptr,    10, false}
		,{  "UnbufferLogs",                      &debug,            nullptr,    10, false}
		,{  "ExAbortOnProblems",                 &exfailabort,      nullptr,    10, false}
		,{  "ExposeOrigin",                      &exporigin,        nullptr,    10, false}
		,{  "LogSubmittedOrigin",                &logxff,           nullptr,    10, false}
		,{  "RecompBz2",                         &recompbz2,        nullptr,    10, false}
		,{  "NetworkTimeout",                    &nettimeout,       nullptr,    10, false}
		,{  "FastTimeout",                       &fasttimeout,      nullptr,    10, false}
		,{  "DisconnectTimeout",                 &discotimeout,     nullptr,    10, false}
		,{  "MinUpdateInterval",                 &updinterval,      nullptr,    10, false}
		,{  "ForwardBtsSoap",                    &forwardsoap,      nullptr,    10, false}
		,{  "KeepExtraVersions",                 &keepnver,         nullptr,    10, false}
		,{  "UseWrap",                           &usewrap,          nullptr,    10, false}
		,{  "FreshIndexMaxAge",                  &maxtempdelay,     nullptr,    10, false}
		,{  "RedirMax",                          &redirmax,         nullptr,    10, false}
		,{  "VfileUseRangeOps",                  &vrangeops,        nullptr,    10, false}
		,{  "ResponseFreezeDetectTime",          &stucksecs,        nullptr,    10, false}
		,{  "ReuseConnections",                  &persistoutgoing,  nullptr,    10, false}
		,{  "PipelineDepth",                     &pipelinelen,      nullptr,    10, false}
		,{  "ExSuppressAdminNotification",       &exsupcount,       nullptr,    10, false}
		,{  "OptProxyTimeout",                   &optproxytimeout,  nullptr,    10, false}
		,{  "MaxDlSpeed",                        &maxdlspeed,       nullptr,    10, false}
		,{  "MaxInresponsiveDlSize",             &maxredlsize,      nullptr,    10, false}
		,{  "OptProxyCheckInterval",             &optProxyCheckInt, nullptr,    10, false}
		,{  "TrackFileUse",		             	 &trackfileuse,		nullptr,    10, false}
        ,{  "ReserveSpace",                      &allocspace, 		nullptr ,   10, false}

        // octal base interpretation of UNIX file permissions
		,{  "DirPerms",                          &dirperms,         nullptr,    8, false}
		,{  "FilePerms",                         &fileperms,        nullptr,    8, false}

		,{ "Verbose", 			nullptr,		"Option is deprecated, ignoring the value." , 10, true}
		,{ "MaxSpareThreadSets",&tpstandbymax, 	"Deprecated option name, mapped to MaxStandbyConThreads", 10, true}
		,{ "OldIndexUpdater",	&oldupdate, 	"Option is deprecated, ignoring the value." , 10, true}
		,{ "Patrace",	&patrace, 				"Don't use in config files!" , 10, false}
		,{ "NoSSLchecks",	&nsafriendly, 		"Disable SSL security checks" , 10, false}
};

#define BARF(msg) throw tStartupException(tSS() << msg)
tProperty n2pTbl[] =
{
{ "Proxy", [](auto key, auto value)
{
	if(value.empty()) proxy_info=tHttpUrl();
	else
	{
		if (!proxy_info.SetHttpUrl(value) || proxy_info.sHost.empty())
		BARF("Invalid proxy specification, aborting...");
	}
	return true;
}, [](bool superUser) -> string
{
	if(!superUser && !proxy_info.sUserPass.empty())
		return string("#");
	return proxy_info.sHost.empty() ? sEmptyString : proxy_info.ToURI(false);
} },
{ "LocalDirs", [](auto key, auto value) -> bool
{
	_ParseLocalDirs(value);
	return !localdirs.empty();
}, [](bool) -> string
{
	string ret;
	for(auto kv : localdirs)
	ret += kv.first + " " + kv.second + "; ";
	return ret;
} },
{ "AllowUserPorts", [](auto key, auto value) -> bool
{
	if(!pUserPorts)
	pUserPorts=new bitset<TCP_PORT_MAX>;
	for(auto slice: tSplitWalk(value, SPACECHARS, false))
	{
		auto s(to_string(slice)); // be zero-terminated
		const char *start(s.c_str());
		char *p(0);
		unsigned long n=strtoul(start, &p, 10);
		if(n>=TCP_PORT_MAX || !p || '\0' != *p || p == start)
		BARF("Bad port in AllowUserPorts: " << start);
		if(n == 0)
		{
			pUserPorts->set();
			break;
		}
		pUserPorts->set(n, true);
	}
	return true;
}, [](bool) -> string
{
	tSS ret;
	if(pUserPorts)
		{
	for(auto i=0; i<TCP_PORT_MAX; ++i)
	ret << (ret.empty() ? "" : ", ") << i;
		}
	return (string) ret;
} },
{ "ConnectProto", [](auto key, auto value) -> bool
{
	int *p = conprotos;
	for (tSplitWalk split(value, SPACECHARS, false); split.Next(); ++p)
	{
		auto val(split.view());
		if (val.empty()) break;

		if (p >= conprotos + _countof(conprotos))
		BARF("Too many protocols specified: " << val);

		if (val == "v6")
		*p = PF_INET6;
		else if (val == "v4")
		*p = PF_INET;
		else
		BARF("IP protocol not supported: " << val);
	}
	return true;
}, [](bool) -> string
{
	string ret(conprotos[0] == PF_INET6 ? "v6" : "v4");
	if(conprotos[0] != conprotos[1])
		ret += string(" ") + (conprotos[1] == PF_INET6 ? "v6" : "v4");
	return ret;
} },
{ "AdminAuth", [](auto key, auto value) -> bool
{
	adminauth.assign(value.data(), value.length());
	adminauthB64=EncodeBase64Auth(adminauth);
	return true;
}, [](bool) -> string
{
	return "#"; // TOP SECRET";
} }
,
{ "ExStartTradeOff", [](auto, auto value) -> bool
{
	exstarttradeoff = strsizeToOfft(to_string(value).c_str());
	return true;
}, [](bool) -> string
{
	return ltos(exstarttradeoff);
} }

 #if SUPPWHASH
 bIsHashedPwd=false;
 }

 else if(CHECKOPTKEY("AdminAuthHash"))
 {
 adminauth=value;
 bIsHashedPwd=true;
 #endif

};

string * GetStringPtr(string_view key) {
	for(auto &ent : n2sTbl)
		if(0==strcasecmp(key, ent.name))
			return ent.ptr;
	return nullptr;
}

int * GetIntPtr(string_view key, int &base) {
	for(auto &ent : n2iTbl)
	{
		if (0 == strcasecmp(key, ent.name))
		{
			if(ent.warn)
				cerr << "Warning, " << key << ": " << ent.warn << endl;
			base = ent.base;
			return ent.ptr;
		}
	}
	return nullptr;
}

tProperty* GetPropPtr(string_view key)
{
	for (auto &ent : n2pTbl)
	{
		if (0 == strcasecmp(key, ent.name))
			return &ent;
	}
	return nullptr;
}

int * GetIntPtr(string_view key)
{
	for(auto &ent : n2iTbl)
	{
		if (0 == strcasecmp(key, ent.name))
			return ent.ptr;
	}
	return nullptr;
}

// shortcut for frequently needed code, opens the config file, reads step-by-step
// and skips comment and empty lines
struct tCfgIter
{
	filereader reader;
	string sLine;
	string sFilename;
	tCfgIter(cmstring &fn) : sFilename(fn)
	{
		reader.OpenFile(fn, false, 1);
	}
	inline bool Next()
	{
		while(reader.GetOneLine(sLine))
		{
			trimFront(sLine);
			if(sLine.empty() || sLine[0] == '#')
				continue;
			return true;
		}
		return false;
	}
};

inline bool qgrep(cmstring &needle, cmstring &file)
{
	for(cfg::tCfgIter itor(file); itor.Next();)
		if(StrHas(itor.sLine, needle))
			return true;
	return false;
}

bool DegradedMode()
{
	return degraded.load();
}

inline void _FixPostPreSlashes(string &val)
{
	// fix broken entries

	if (val.empty() || val.at(val.length()-1) != '/')
		val.append("/");
	if (val.at(0) != '/')
		val.insert(0, "/", 1);
}


inline decltype(repoparms)::iterator GetRepoEntryRef(const string & sRepName)
{
	auto it = repoparms.find(sRepName);
	if(repoparms.end() != it)
		return it;
	// strange, should not happen
	repoparms[sRepName].m_pHooks.reset();
	return repoparms.find(sRepName);
}

tStrDeq ExpandFileTokens(string_view token)
{
	auto sPath = token.substr(5);
	if (sPath.empty())
		BARF("Bad file spec for repname, file:?");
	bool bAbs = IsAbsolute(sPath);
	if (suppdir.empty() || bAbs)
	{
		return ExpandFilePattern(
				bAbs ? to_string(sPath) : (confdir + sPathSep + to_string(sPath))
						, true);
	}
	auto pat = PathCombine(confdir, sPath);
	StrSubst(pat, "//", "/");
	auto res = ExpandFilePattern(pat, true);
	if (res.size() == 1 && !Cstat(res.front()))
		res.clear(); // not existing, wildcard returned
	pat = PathCombine(suppdir, sPath);
	StrSubst(pat, "//", "/");
	auto suppres = ExpandFilePattern(pat, true);
	if (suppres.size() == 1 && !Cstat(suppres.front()))
		return res; // errrr... done here
	// merge them
	tStrSet dupeFil;
        for(const auto& s: res)
#ifdef COMPATGCC47
           dupeFil.insert(GetBaseName(s));
#else
        dupeFil.emplace(GetBaseName(s));
#endif
	for(const auto& s: suppres)
		if(!ContHas(dupeFil, GetBaseName(s)))
			res.emplace_back(s);
	return res;
}


struct tHookHandler: public IHookHandler, public base_with_mutex
{
	string cmdRel, cmdCon;
	time_t downDuration, downTimeNext;

	int m_nRefCnt;

	tHookHandler(cmstring&) :
		downDuration(30), downTimeNext(0), m_nRefCnt(0)
	{
//		cmdRel = "logger JobRelease/" + name;
//		cmdCon = "logger JobConnect/" + name;
	}
	virtual void OnRelease() override
	{
		setLockGuard;
		if (0 >= --m_nRefCnt)
		{
			//system(cmdRel.c_str());
			downTimeNext = ::time(0) + downDuration;
			cleaner::GetInstance().ScheduleFor(downTimeNext, cleaner::TYPE_ACFGHOOKS);
		}
	}
	virtual void OnAccess() override
	{
		setLockGuard;
		if (0 == m_nRefCnt++)
		{
			if(downTimeNext) // huh, already ticking? reset
				downTimeNext=0;
			else if(system(cmdCon.c_str()))
				log::err(tSS() << "Warning: " << cmdCon << " returned with error code.");
		}
	}
};


cmstring & GetMimeType(cmstring &path)
{
	{
		lockguard g(mimemap);
		static bool inited = false;
		if (!inited)
		{
			inited = true;
			for (tCfgIter itor("/etc/mime.types"); itor.Next();)
			{
				// # regular types:
				// text/plain             asc txt text pot brf  # plain ascii files
				string mimetype;
				for(auto token: tSplitWalk(itor.sLine, SPACECHARS, false))
				{
					if(token.starts_with('#'))
						break;
					if(mimetype.empty())
					{
						mimetype=to_string(token);
						continue;
					}
					mimemap[to_string(token)] = mimetype;
				}
			}
		}
	}

	tStrPos dpos = path.find_last_of('.');
	if (dpos != stmiss)
	{
		NoCaseStringMap::const_iterator it = cfg::mimemap.find(path.substr(
				dpos + 1));
		if (it != cfg::mimemap.end())
			return it->second;
	}
	// try some educated guess... assume binary if we are sure, text if we are almost sure
	static cmstring os("application/octet-stream"), tp("text/plain");
	filereader f;
	if(f.OpenFile(path, true))
	{
		size_t maxLen = std::min(size_t(255), f.GetSize());
		for(unsigned i=0; i< maxLen; ++i)
		{
			if(!isascii((uint) *(f.GetBuffer()+i)))
				return os;
		}
		return tp;
	}
	return sEmptyString;
}


void GetRepNameAndPathResidual(const tHttpUrl & in, tRepoResolvResult &result)
{
	// get all the URLs matching THE HOSTNAME
	auto rangeIt=mapUrl2pVname.find(in.sHost+":"+in.GetPort());
	if(rangeIt == mapUrl2pVname.end())
		return;
	
	tStrPos bestMatchLen(0);
	auto pBestHit = repoparms.end();
		
	// now find the longest directory part which is the suffix of requested URL's path
	for (auto& repo : rangeIt->second)
	{
		// rewrite rule path must be a real prefix
		// it's also surrounded by /, ensured during construction
		const string & prefix=repo.first; // path of the rewrite entry
		tStrPos len=prefix.length();
		if (len>bestMatchLen && in.sPath.size() > len && 0==in.sPath.compare(0, len, prefix))
		{
			bestMatchLen=len;
			pBestHit=repo.second;
		}
	}
		
	if(pBestHit != repoparms.end())
	{
		result.psRepoName = & pBestHit->first;
		result.sRestPath = in.sPath.substr(bestMatchLen);
		result.repodata = & pBestHit->second;
	}
}

const tRepoData * GetRepoData(cmstring &vname)
{
	auto it=repoparms.find(vname);
	if(it==repoparms.end())
		return nullptr;
	return & it->second;
}


void ShutDown()
{
	mapUrl2pVname.clear();
	repoparms.clear();
}

void dump_config(bool includeDelicate)
{
	ostream &cmine(cout);

	for (auto& n2s : n2sTbl)
		if (n2s.ptr)
			cmine << n2s.name << " = " << *n2s.ptr << endl;

	if (cfg::debug >= log::LOG_DEBUG)
	{
		cerr << "escaped version:" << endl;
		for (const auto& n2s : n2sTbl)
		{
			if (n2s.ptr)
			{
				cerr << n2s.name << " = ";
				for (const char *p = n2s.ptr->c_str(); *p; p++)
				{
					if ('\\' == *p)
						cmine << "\\\\";
					else
						cmine << *p;
				}
				cmine << endl;
			}
		}
	}

	for (const auto& n2i : n2iTbl)
		if (n2i.ptr && !n2i.hidden)
			cmine << n2i.name << " = " << *n2i.ptr << endl;

	for (const auto& x : n2pTbl)
	{
		auto val(x.get(includeDelicate));
		if(startsWithSz(val, "#")) continue;
		cmine << x.name << " = " << val << endl;
	}

#ifndef DEBUG
	if (cfg::debug >= log::LOG_DEBUG)
		cerr << "\n\nAdditional debugging information not compiled in.\n\n";
#endif

#if 0 //def DEBUG
#warning adding hook control pins
	for(tMapString2Hostivec::iterator it = repoparms.begin();
			it!=repoparms.end(); ++it)
	{
		tHookHandler *p = new tHookHandler(it->first);
		p->downDuration=10;
		p->cmdCon = "logger wanna/connect";
		p->cmdRel = "logger wanna/disconnect";
		it->second.m_pHooks = p;
	}

	if(debug == -42)
	{
		/*
		 for(tMapString2Hostivec::const_iterator it=mapRepName2Backends.begin();
		 it!=mapRepName2Backends.end(); it++)
		 {
		 for(tRepoData::const_iterator jit=it->second.begin();
		 jit != it->second.end(); jit++)
		 {
		 cout << jit->ToURI() <<endl;
		 }
		 }

		 for(tUrl2RepIter it=mapUrl2pVname.begin(); it!=mapUrl2pVname.end(); it++)
		 {
		 cout << it->first.ToURI(false) << " ___" << *(it->second) << endl;
		 }

		 exit(1);
		 */
	}
#endif
}

//! @brief Fires hook callbacks in the background thread
time_t BackgroundCleanup()
{
	time_t ret(END_OF_TIME), now(time(0));
	for (const auto& parm : repoparms)
	{
		if (!parm.second.m_pHooks)
			continue;

		auto hooks = static_cast<tHookHandler*>(parm.second.m_pHooks.get());

		lockguard g(*hooks);
		if (hooks->downTimeNext)
		{
			if (hooks->downTimeNext <= now) // time to execute
			{
				if(cfg::debug & log::LOG_MORE)
					log::misc(hooks->cmdRel, 'X');
				if(cfg::debug & log::LOG_FLUSH)
					log::flush();

				if(system(hooks->cmdRel.c_str()))
					log::err(tSS() << "Warning: " << hooks->cmdRel << " returned with error code.");
				hooks->downTimeNext = 0;
			}
			else // in future, use the soonest time
				ret = min(ret, hooks->downTimeNext);
		}
	}
	return ret;
}

acmutex authLock;

int CheckAdminAuth(LPCSTR auth)
{
	if(cfg::adminauthB64.empty())
		return 0;
	if(!auth || !*auth)
		return 1; // request it from user
	if(strncmp(auth, "Basic", 5))
		return -1; // looks like crap
	auto p=auth+5;
	while(*p && isspace((uint) *p)) ++p;

#ifndef SUPPWHASH
	return adminauthB64.compare(p) == 0 ? 0 : 1;

#else

	if(!bIsHashedPwd)
		return adminauth.compare(p) == 0 ? 0 : 1;

#ifndef HAVE_SSL
#warning You really want to add SSL support in order to support hashed passwords
	return -1;
#endif
	acbuf bufDecoded;
	if(!DecodeBase64(p, bufDecoded))
		return -1;
#if 0
	// there is always a char reserved
	cerr << "huhu, user sent: " << bufDecoded.c_str() <<endl;
#endif

	string usersauth(bufDecoded.rptr(), bufDecoded.size());
	auto poscol=usersauth.find(':');
	if(poscol==0 || poscol==stmiss || poscol+1==usersauth.size())
		return 1;

	// ok, try to match against our hash, first copy user and salt from config
	// always calculate the thing and compare the user and maybe hash later
	// attacker should not gain any knowledge from faster abort (side channel...)
	lockguard g(&authLock);
	string testHash=adminauth.substr(0, poscol+9);
	if(!AppendPasswordHash(testHash, usersauth.data()+poscol+1, usersauth.size()-poscol+1))
		return 1;
	if(testHash == adminauth)
	{
		// great! Cache it!
		adminauth = p;
		bIsHashedPwd = false;
		return 0;
	}
	return 1;
#endif
}

static bool proxy_failstate = false;
acmutex proxy_fail_lock;
const tHttpUrl* GetProxyInfo()
{
	if(proxy_info.sHost.empty())
		return nullptr;

	static time_t last_check=0;

	lockguard g(proxy_fail_lock);
	time_t now = time(nullptr);
	time_t sinceCheck = now - last_check;
	if(sinceCheck > optProxyCheckInt)
	{
		last_check = now;
		if(optProxyCheckCmd.empty())
			proxy_failstate = false;
		else
			proxy_failstate = (bool) system(optProxyCheckCmd.c_str());
	}

	return proxy_failstate ? nullptr : &proxy_info;
}

void MarkProxyFailure()
{
	lockguard g(proxy_fail_lock);
	if(optProxyCheckInt <= 0) // urgs, would never recover
		return;
	proxy_failstate = true;
}

static const std::string SIGNATURE_PREFIX = "a" ACVERSION "c"
/* repro builds
#ifdef DEBUG
	","	__DATE__ "," __TIME__
#endif
*/
		;

// information with limited livespan - consumed and released by PostProcConfig
struct tConfigBuilder::tImpl
{
	std::map<std::string, std::vector<std::string>> remap_lines;
	bool m_bIgnoreErrors, m_bAssertReadableFiles;
	// user parameters, with flag == true for directory
	std::deque<std::pair<std::string,bool>> m_input;

	//bool AppendPasswordHash(mstring &stringWithSalt, LPCSTR plainPass, size_t passLen);

	class tMappingValidator
		{
			std::string m_oldSig;
			Cstat x;
			std::unique_ptr<csumBase> m_summer;
			enum { INIT, VALIDATING, NEW_STUFF, MISMATCH, FINISHED } m_mode = INIT;

			void init_summer()
			{
				m_summer = csumBase::GetChecker(CSTYPES::MD5);
				// pointless, better have reproducible tests without constant updates
				// m_summer->add(SIGNATURE_PREFIX);
			}
public:
			const std::string &m_name;

			explicit tMappingValidator(const std::string& name) : m_name(name)
			{
			}

			bool IsDryRun()
			{
				return m_mode == VALIDATING;
			}
			void ReportString(string_view s)
			{
				m_summer->add(s);
			}
			void ReportFile(cmstring& path, Cstat* stinfo)
			{
				m_summer->add(path);
				if(!stinfo)
				{
					stinfo=&x;
					x.reset(path);
				}
				m_summer->add(";");
	#define PUN2BS(what) (const uint8_t*) (const char*) & what, sizeof(what)
				m_summer->add(PUN2BS(stinfo->st_size));
				m_summer->add(PUN2BS(stinfo->st_ino));
				m_summer->add(PUN2BS(stinfo->st_dev));
				m_summer->add(PUN2BS(stinfo->st_mtim));
				if(access(path.c_str(), R_OK))
					m_summer->add("ntrdbl");
			}
			/**
			 * Finish the input data flow and analyze the resulting signature.
			 * @return true if needing another run
			 */
			bool Next(IDbManager& dbman)
			{
				switch(m_mode)
				{
				case INIT:
				{
					init_summer();
					try
					{
						m_oldSig = dbman.GetMappingSignature(m_name);
						m_mode = startsWith(m_oldSig, SIGNATURE_PREFIX) ? VALIDATING : MISMATCH;
					}
					catch (const SQLite::Exception &e)
					{
						m_mode = NEW_STUFF;
					}
					break;
				}
				case VALIDATING:
				{
					auto mx = m_summer->finish().to_string();
					if (endsWith(m_oldSig, mx))
						m_mode = FINISHED;
					else
					{
						init_summer();
						m_mode = MISMATCH;
					}
					break;
				}
				case MISMATCH:
				case NEW_STUFF:
					dbman.StoreMappingSignature(m_name, SIGNATURE_PREFIX + m_summer->finish().to_string());
					__just_fall_through;
				default:
					m_mode = FINISHED;
					break;
				}
				return FINISHED != m_mode;
			}
		};

	void ReadOneConfFile(const string & szFilename)
	{
		tCfgIter itor(szFilename);
		if(!itor.reader.IsGood())
		{
			if(!m_bAssertReadableFiles)
				return;
			throw tStartupException(string("Cannot read ") + szFilename);
		}

		while(itor.Next())
		{
			// XXX: To something about escaped/quoted version
			tStrPos pos=itor.sLine.find('#');
			if(stmiss != pos)
				itor.sLine.erase(pos);
	#warning catch exception
			SetOption(itor.sLine);
		}
	}

	void AddRemapInfo(bool bAsBackend, const string_view token,
			tMappingValidator &ctx)
	{
		const auto& repname = ctx.m_name;

		if (0 != token.compare(0, 5, "file:"))
		{
			// ok, plain word
			ctx.ReportString(token);
			if(ctx.IsDryRun()) return;

			tHttpUrl url;
			if(! url.SetHttpUrl(token))
				BARF(to_string(token) + " <-- bad URL detected");
			_FixPostPreSlashes(url.sPath);

			if (bAsBackend)
				repoparms[repname].m_backends.emplace_back(url);
			else
				mapUrl2pVname[url.sHost+":"+url.GetPort()].emplace_back(
						url.sPath, GetRepoEntryRef(repname));
		}
		else
		{
			auto func = [&](const string & x)
					{
				return bAsBackend ? ReadBackendsFile(x, ctx) : ReadRewriteFile(x, ctx);
			};

			unsigned count = 0;

			for(auto& src : ExpandFileTokens(token))
				count += func(src);

			if(0 == count)
			{
				for(auto& src : ExpandFileTokens(to_string(token) + ".default"))
					count += func(src);
			}
			if(!count && !m_bIgnoreErrors)
				BARF("WARNING: No configuration was read from " << token);
		}
	}

	unsigned ReadBackendsFile(const string & sFile, tMappingValidator &ctx)
	{
		unsigned nAddCount=0;
		string key, val;
		tHttpUrl entry;

		const auto& sRepName = ctx.m_name;
		ctx.ReportFile(sFile, nullptr);
		if(ctx.IsDryRun()) return 1;

		tCfgIter itor(sFile);
#warning fixme, magic number, also below
		if(debug & 6)
			cerr << "Reading backend file: " << sFile <<endl;

		if(!itor.reader.IsGood())
		{
			if(debug & 6)
				cerr << "No backend data found, file ignored."<<endl;
			return 0;
		}

		while(itor.Next())
		{
			if(debug & log::LOG_DEBUG)
				cerr << "Backend URL: " << itor.sLine <<endl;

			trimBack(itor.sLine);

			if( entry.SetHttpUrl(itor.sLine)
					||	( itor.sLine.empty() && ! entry.sHost.empty() && ! entry.sPath.empty()) )
			{
				_FixPostPreSlashes(entry.sPath);
	#ifdef DEBUG
				cerr << "Backend: " << sRepName << " <-- " << entry.ToURI(false) <<endl;
	#endif
				repoparms[sRepName].m_backends.emplace_back(entry);
				nAddCount++;
				entry.clear();
			}
			else if(ParseKeyValLine(itor.sLine, key, val))
			{
				if(keyEq("Site", key))
					entry.sHost=val;
				else if(keyEq("Archive-http", key) || keyEq("X-Archive-http", key))
					entry.sPath=val;
			}
			else
			{
				BARF("Bad backend description, around line " << sFile << ":"
						<< itor.reader.GetCurrentLine());
			}
		}
		return nAddCount;
	}
	/* This parses also legacy files, i.e. raw RFC-822 formated mirror catalogue from the
	 * Debian archive maintenance repository.
	 */
	unsigned ReadRewriteFile(const string & sFile, tMappingValidator &ctx)
	{
		unsigned nAddCount=0;

		const auto& sRepName = ctx.m_name;
		ctx.ReportFile(sFile, nullptr);
		if(ctx.IsDryRun())
			return 1;

		filereader reader;
		if(debug>4)
			cerr << "Reading rewrite file: " << sFile <<endl;
		reader.OpenFile(sFile, false, 1);
		if(!reader.IsGood())
		{
			if(m_bIgnoreErrors)
				return 0;
			throw tStartupException(tSS() << "Error reading rewrite file " << sFile);
		}

		tStrVec hosts, paths;
		string sLine, key, val;
		tHttpUrl url;

		while (reader.GetOneLine(sLine))
		{
			trimFront(sLine);

			if (0 == startsWithSz(sLine, "#"))
				continue;

			if (url.SetHttpUrl(sLine))
			{
				_FixPostPreSlashes(url.sPath);

				mapUrl2pVname[url.sHost + ":" + url.GetPort()].emplace_back(url.sPath,
						GetRepoEntryRef(sRepName));
	#ifdef DEBUG
				cerr << "Mapping: " << url.ToURI(false) << " -> " << sRepName << endl;
	#endif

				++nAddCount;
				continue;
			}

			// otherwise deal with the complicated RFC-822 format for legacy reasons

			if (sLine.empty()) // end of block, eof, ... -> commit it
			{
				if (hosts.empty() && paths.empty())
					continue; // dummy run or whitespace in a URL style list
				if ( !hosts.empty() && paths.empty())
				{
					cerr << "Warning, missing path spec for the site " << hosts[0] <<", ignoring mirror."<< endl;
					continue;
				}
				if ( !paths.empty() && hosts.empty())
				{
					BARF("Parse error, missing Site: field around line "
							<< sFile << ":"<< reader.GetCurrentLine());
				}
				for (const auto& host : hosts)
				{
					for (const auto& path : paths)
					{
						//mapUrl2pVname[*itHost+*itPath]= &itHostiVec->first;
						tHttpUrl url;
						url.sHost=host;
						url.sPath=path;
						mapUrl2pVname[url.sHost+":"+url.GetPort()].emplace_back(url.sPath,
								GetRepoEntryRef(sRepName));

	#ifdef DEBUG
							cerr << "Mapping: "<< host << path
							<< " -> "<< sRepName <<endl;
	#endif

							++nAddCount;
					}
				}
				hosts.clear();
				paths.clear();
				continue;
			}

			if(!ParseKeyValLine(sLine, key, val))
			{
				BARF("Error parsing rewrite definitions, around line "
						<< sFile << ":"<< reader.GetCurrentLine() << " : " << sLine);
			}

			// got something, interpret it...
			if( keyEq("Site", key) || keyEq("Alias", key) || keyEq("Aliases", key))
				Tokenize(val, SPACECHARS, hosts, true);

			if(keyEq("Archive-http", key) || keyEq("X-Archive-http", key))
			{
				// help STL saving some memory
				if(sPopularPath==val)
					paths.emplace_back(sPopularPath);
				else
				{
					_FixPostPreSlashes(val);
					paths.emplace_back(val);
				}
				continue;
			}
		}

		return nAddCount;
	}

	void _AddHooksFile(cmstring& vname)
	{
		auto hfile = cfg::confdir+"/"+vname+".hooks";
		tCfgIter itor(hfile);
		if(!itor.reader.IsGood())
		{
			// hooks files are optional. XXX: make that an explicit option and fail explicitly.
			return;

			/*
			if(m_bIgnoreErrors)
				return;
			throw tStartupException("Error reading hooks file " + hfile);
			*/
		}
		auto hs = make_unique<tHookHandler>(vname);
		string_view key,val;
		while (itor.Next())
		{
			ParseOptionLine(itor.sLine, key, val);

			if (strcasecmp("PreUp", key) == 0)
			{
				hs->cmdCon = val;
			}
			else if (strcasecmp("Down", key) == 0)
			{
				hs->cmdRel = val;
			}
			else if (strcasecmp("DownTimeout", key) == 0)
			{
				errno = 0;
				// XXX: maybe just forcibly terminate it because we know that it came from a string and data is writable. Or eventually make a better strtoul wrapper.
				unsigned n = strtoul(to_string(val).c_str(), nullptr, 10);
				if (!errno)
					hs->downDuration = n;
			}
		}
		repoparms[vname].m_pHooks = move(hs);
	}

	void AddRemapFlag(string_view token, tMappingValidator& ctx)
	{

		const string &repname = ctx.m_name;
		ctx.ReportString(token);
		if(ctx.IsDryRun()) return;

		string_view key, value;
		ParseOptionLine(token, key, value);

		tRepoData &where = repoparms[repname];

		if(key=="keyfile")
		{
			if(value.empty())
				return;
			if (cfg::debug & log::LOG_FLUSH)
				cerr << "Fatal keyfile for " <<repname<<": "<<value <<endl;

			where.m_keyfiles.emplace_back(value);
		}
		else if(key=="deltasrc")
		{
			if(value.empty())
				return;

			bool addSlash=!endsWithSzAr(value, "/");

			if(!where.m_deltasrc.SetHttpUrl(addSlash ? (to_string(value)+"/") : value))
				cerr << "Couldn't parse Debdelta source URL, ignored " <<value <<endl;
		}
		else if(key=="proxy")
		{
			static std::list<tHttpUrl> alt_proxies;
			tHttpUrl cand;
			if(value.empty() || cand.SetHttpUrl(value))
			{
				alt_proxies.emplace_back(cand);
				where.m_pProxy = & alt_proxies.back();
			}
			else
			{
				cerr << "Warning, failed to parse proxy setting " << value << " , "
						<< endl << "ignoring it" <<endl;
			}
		}
	}


#warning add a --trace-config option to help identifying the problems. However, shall warn or abort if user-id is not matching
#warning and document the trace mode
void SetOption(acng::string_view sLine)
{
	string_view key, value;

	ParseOptionLine(sLine, key, value);

	string * psTarget;
	int * pnTarget;
	tProperty * ppTarget;
	int nNumBase(10);

	if(key.length() > 6 && 0 == strncasecmp(key.data(), "remap-", 6))
	{
		remap_lines[to_string(key.substr(6))].emplace_back(value);
	}
	else if ( nullptr != (psTarget = GetStringPtr(key)))
	{
		*psTarget=value;
	}
	else if ( nullptr != (pnTarget = GetIntPtr(key, nNumBase)))
	{
		// temp. copy is ok here since a number string is most likely SSOed
		auto temp(to_string(value));
		const char *pStart=temp.c_str();
		if(! *pStart)
				throw tStartupException(tSS() << "Missing value for " << key << " option!");
			errno = 0;
			char *pEnd(nullptr);
			long nVal = strtol(pStart, &pEnd, nNumBase);
			if (errno)
				throw tStartupException(
						tSS() << "Invalid number for " << key << ":" << tErrnoFmter());

			if (RESERVED_DEFVAL == nVal)
				throw tStartupException(
						tSS() << "Bad value for " << key << " (protected value, use another one)");

			if (*pEnd)
			{
				throw tStartupException(
						tSS() << "Bad value for " << key << " option or found trailing garbage: "
								<< pEnd);
			}

			*pnTarget=nVal;
	}
	else if ( nullptr != (ppTarget = GetPropPtr(key)))
	{
		ppTarget->set(key, value);
	}
	else
	{
		if(!m_bIgnoreErrors)
			throw tStartupException(tSS() << "Warning, unknown configuration directive: " << key);
	}
}


void ParseOptionLine(string_view sLine, string_view &key, string_view &val)
{
	string::size_type posCol = sLine.find(":");
	string::size_type posEq = sLine.find("=");
	if (posEq==stmiss && posCol==stmiss)
	{
		if(m_bIgnoreErrors)
			return;
		throw tStartupException(tSS() << "Not a valid configuration directive: " << sLine);
	}
	string::size_type pos;
	if (posEq!=stmiss && posCol!=stmiss)
		pos=min(posEq,posCol);
	else if (posEq!=stmiss)
		pos=posEq;
	else
		pos=posCol;

	key=sLine.substr(0, pos);
	trimBoth(key);
	val=sLine.substr(pos+1);
	trimBoth(val);
	if(key.empty())
		return; // XXX: report it?

	if(endsWithSzAr(val, "\\"))
		cerr << "Warning: multilines are not supported, consider using \\n." <<endl;
}

void LoadMappings(IDbManager& dbman)
{
	for (const auto &it : remap_lines)
	{
		const auto &vname = it.first;
		if (vname.empty())
		{
			if (!m_bIgnoreErrors)
				throw tStartupException("Bad repository name, check Remap-... directives");
			continue;
		}

		for(tMappingValidator ctx(vname); ctx.Next(dbman);)
		{

		for (const auto &remapLine : it.second)
		{
			ctx.ReportString(remapLine);

			enum xtype
			{
				PREFIXES, BACKENDS, FLAGS
			} type = xtype(PREFIXES - 1);
			tSplitWalk tokenSeq(remapLine, ";", true);
			for (auto remapToken : tokenSeq)
			{
				type = xtype(type + 1);
				tSplitWalk itemSeq(remapToken, SPACECHARS, false);
				for (auto s : itemSeq)
				{
					if (s.empty())
						continue;
					if (s.starts_with('#'))
						goto next_remap_line;

					switch (type)
					{
					case PREFIXES:
						AddRemapInfo(false, s, ctx);
						break;
					case BACKENDS:
						AddRemapInfo(true, s, ctx);
						break;
					case FLAGS:
						AddRemapFlag(s, ctx);
						break;
					}
				}
			}
			if (type < PREFIXES) // no valid tokens?
			{
				if (!m_bIgnoreErrors)
					throw tStartupException(
							std::string("Invalid entry, no valid configuration for Remap-")
									+ ": " + remapLine);
				continue;
			}
			next_remap_line: ;
		}
		}
		_AddHooksFile(vname);
	}
}

void ReadConfigDirectory(cmstring& sPath)
{
	dump_proc_status();
	char buf[PATH_MAX];
	if(!realpath(sPath.c_str(), buf))
		BARF("Failed to open config directory");

	confdir=buf; // pickup the last config directory

#if defined(HAVE_WORDEXP) || defined(HAVE_GLOB)
	for(const auto& src: ExpandFilePattern(confdir+SZPATHSEP "*.conf", true))
		ReadOneConfFile(src);

#else
	ReadOneConfFile(confdir+SZPATHSEP"acng.conf");
#endif
	dump_proc_status();
	if(debug & log::LOG_DEBUG)
	{
		unsigned nUrls=0;
		for(const auto& x: mapUrl2pVname)
			nUrls+=x.second.size();

		if(debug & log::LOG_DEBUG_MORE)
			cerr << "Loaded " << repoparms.size() << " backend descriptors\nLoaded mappings for "
				<< mapUrl2pVname.size() << " hosts and " << nUrls<<" paths\n";
	}
}

#warning catch exception on all users
void Build(IDbManager& dbman)
{
	for(const auto& inputThing: m_input)
	{
		if(inputThing.second)
			ReadConfigDirectory(inputThing.first);
		else
			SetOption(inputThing.first);
	}

	mapUrl2pVname.rehash(mapUrl2pVname.size());

	if(port.empty()) // heh?
		port=ACNG_DEF_PORT;

	if(connectPermPattern == "~~~")
	   connectPermPattern="^(bugs\\.debian\\.org|changelogs\\.ubuntu\\.com):443$";

	// let's also apply the umask to the directory permissions
	{
		mode_t mask = umask(0);
		umask(mask); // restore it...
		dirperms &= ~mask;
		fileperms &= ~mask;
	}

    // postprocessing

#ifdef FORCE_CUSTOM_UMASK
	if(!sUmask.empty())
	{
		mode_t nUmask=0;
		if(sUmask.size()>4)
			BARF("Invalid umask length\n");
		for(unsigned int i=0; i<sUmask.size(); i++)
		{
			unsigned int val = sUmask[sUmask.size()-i-1]-'0';
			if(val>7)
				BARF("Invalid umask value\n");
			nUmask |= (val<<(3*i));

		}
		//cerr << "Got umask: " << nUmask <<endl;
		umask(nUmask);
	}
#endif

   if(cachedir.empty() || cachedir[0] != CPATHSEP)
   {
	   cerr << "Warning: Cache directory unknown or not absolute, running in degraded mode!" << endl;
	   degraded=true;
   }
   try
   {
	   rex::CompileExpressions();
   }
   catch(const exception& ex)
   {
	   BARF("An error occurred while compiling file type regular expressions: " << ex.what());
   }

   if(cfg::tpthreadmax < 0)
	   cfg::tpthreadmax = MAX_VAL(int);

   // get rid of duplicated and trailing slash(es)
	for(tStrPos pos; stmiss != (pos = cachedir.find(SZPATHSEP SZPATHSEP )); )
		cachedir.erase(pos, 1);

	cacheDirSlash=cachedir+CPATHSEP;

   if(!pidfile.empty() && pidfile.at(0) != CPATHSEP)
	   BARF("Pid file path must be absolute, terminating...");

   if(!cfg::agentname.empty())
	   cfg::agentheader=string("User-Agent: ")+cfg::agentname + "\r\n";

   stripPrefixChars(cfg::reportpage, '/');

   trimBack(cfg::requestapx);
   trimFront(cfg::requestapx);
   if(!cfg::requestapx.empty())
	   cfg::requestapx = unEscape(cfg::requestapx);

   // create working paths before something else fails somewhere
   if(!udspath.empty())
	   mkbasedir(cfg::udspath);
   if(!cachedir.empty())
	   mkbasedir(cfg::cachedir);
   if(! pidfile.empty())
	   mkbasedir(cfg::pidfile);

   if(nettimeout < 5) {
	   cerr << "Warning: NetworkTimeout value too small, using: 5." << endl;
	   nettimeout = 5;
   }
   if(fasttimeout < 0)
   {
	   fasttimeout = 0;
   }

   if(RESERVED_DEFVAL == forwardsoap)
	   forwardsoap = !forcemanaged;

   if(RESERVED_DEFVAL == exsupcount)
	   exsupcount = (extreshhold >= 5);

#ifdef _SC_NPROCESSORS_ONLN
	numcores = (int) sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(_SC_NPROC_ONLN)
	numcores = (int) sysconf(_SC_NPROC_ONLN);
#endif

   if(!rex::CompileUncExpressions(rex::NOCACHE_REQ,
		   tmpDontcacheReq.empty() ? tmpDontcache : tmpDontcacheReq)
   || !rex::CompileUncExpressions(rex::NOCACHE_TGT,
		   tmpDontcacheTgt.empty() ? tmpDontcache : tmpDontcacheTgt))
   {
	   BARF("An error occurred while compiling regular expression for non-cached paths!");
   }
   tmpDontcache.clear();
   tmpDontcacheTgt.clear();
   tmpDontcacheReq.clear();

   if(usewrap == RESERVED_DEFVAL)
   {
	   usewrap=(qgrep("apt-cacher-ng", "/etc/hosts.deny")
			   || qgrep("apt-cacher-ng", "/etc/hosts.allow"));
#ifndef HAVE_LIBWRAP
	   cerr << "Warning: configured to use libwrap filters but feature is not built-in." <<endl;
#endif
   }

   if(maxtempdelay<0)
   {
	   cerr << "Invalid maxtempdelay value, using default" <<endl;
	   maxtempdelay=27;
   }

   if(redirmax == RESERVED_DEFVAL)
	   redirmax = forcemanaged ? 0 : REDIRMAX_DEFAULT;

   if(!persistoutgoing)
	   pipelinelen = 1;

   if(!pipelinelen) {
	   cerr << "Warning, remote pipeline depth of 0 makes no sense, assuming 1." << endl;
	   pipelinelen = 1;
   }

   LoadMappings(dbman);
}

}; // tConfigBuilder::Impl

tConfigBuilder::tConfigBuilder(bool ignoreAllErrors, bool ignoreFileReadErrors)
{
	m_pImpl = new tImpl();
	m_pImpl->m_bIgnoreErrors = ignoreAllErrors;
	m_pImpl->m_bAssertReadableFiles = !ignoreFileReadErrors;
}

tConfigBuilder::~tConfigBuilder()
{
	delete m_pImpl;
}

tConfigBuilder& tConfigBuilder::AddOption(const mstring &line)
{
	m_pImpl->m_input.emplace_back(make_pair(line, false));
	return *this;
}

tConfigBuilder& tConfigBuilder::AddConfigDirectory(cmstring &sDirName)
{
	m_pImpl->m_input.emplace_back(make_pair(sDirName, true));
	return *this;
}

void tConfigBuilder::Build(IDbManager& dbman)
{
	m_pImpl->Build(dbman);
}

} // namespace acfg
}
