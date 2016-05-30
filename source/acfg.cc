
#include "debug.h"

#include "acfg.h"
#include "meta.h"
#include "filereader.h"
#include "fileio.h"
#include "lockable.h"
#include "cleaner.h"

#include <regex.h>

#include <iostream>
#include <fstream>
#include <deque>
#include <algorithm>
#include <list>
#include <unordered_map>


using namespace std;

// hint to use the main configuration excluding the complex directives
//bool g_testMode=false;

bool bIsHashedPwd=false;

#define BARF(x) {if(!g_bQuiet) { cerr << x << endl;} exit(EXIT_FAILURE); }
#define BADSTUFF_PATTERN "\\.\\.($|%|/)"

namespace rechecks
{
bool CompileExpressions();
}


namespace acfg {

bool g_bQuiet=false, g_bNoComplex=false;

// internal stuff:
string sPopularPath("/debian/");
string tmpDontcache, tmpDontcacheReq, tmpDontcacheTgt, optProxyCheckCmd;
int optProxyCheckInt = 99;

tStrMap localdirs;
static class : public base_with_mutex, public NoCaseStringMap {} mimemap;

std::bitset<TCP_PORT_MAX> *pUserPorts = nullptr;

tHttpUrl proxy_info;

struct MapNameToString
{
	const char *name; mstring *ptr;
};

struct MapNameToInt
{
	const char *name; int *ptr;
	const char *warn; uint8_t base;
};

struct tProperty
{
	const char *name;
	std::function<bool(cmstring& key, cmstring& value)> set;
	std::function<mstring(bool superUser)> get; // returns a string value. A string starting with # tells to skip the output
};

#ifndef MINIBUILD

// predeclare some
void _ParseLocalDirs(cmstring &value);
void AddRemapInfo(bool bAsBackend, const string & token, const string &repname);
void AddRemapFlag(const string & token, const string &repname);
void _AddHooksFile(cmstring& vname);

unsigned ReadBackendsFile(const string & sFile, const string &sRepName);
unsigned ReadRewriteFile(const string & sFile, cmstring& sRepName);

map<cmstring, tRepoData> repoparms;
typedef decltype(repoparms)::iterator tPairRepoNameData;
// maps hostname:port -> { <pathprefix,repopointer>, ... }
std::unordered_map<string, list<pair<cmstring,tPairRepoNameData>>> mapUrl2pVname;

MapNameToString n2sTbl[] = {
		{   "Port",                    &port}
		,{  "CacheDir",                &cachedir}
		,{  "LogDir",                  &logdir}
		,{  "SupportDir",              &suppdir}
		,{  "SocketPath",              &fifopath}
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
		{   "Debug",                             &debug,            nullptr,    10}
		,{  "OfflineMode",                       &offlinemode,      nullptr,    10}
		,{  "ForeGround",                        &foreground,       nullptr,    10}
		,{  "ForceManaged",                      &forcemanaged,     nullptr,    10}
		,{  "StupidFs",                          &stupidfs,         nullptr,    10}
		,{  "VerboseLog",                        &verboselog,       nullptr,    10}
		,{  "ExTreshold",                        &extreshhold,      nullptr,    10}
		,{  "MaxStandbyConThreads",              &tpstandbymax,     nullptr,    10}
		,{  "MaxConThreads",                     &tpthreadmax,      nullptr,    10}
		,{  "DnsCacheSeconds",                   &dnscachetime,     nullptr,    10}
		,{  "UnbufferLogs",                      &debug,            nullptr,    10}
		,{  "ExAbortOnProblems",                 &exfailabort,      nullptr,    10}
		,{  "ExposeOrigin",                      &exporigin,        nullptr,    10}
		,{  "LogSubmittedOrigin",                &logxff,           nullptr,    10}
		,{  "RecompBz2",                         &recompbz2,        nullptr,    10}
		,{  "NetworkTimeout",                    &nettimeout,       nullptr,    10}
		,{  "MinUpdateInterval",                 &updinterval,      nullptr,    10}
		,{  "ForwardBtsSoap",                    &forwardsoap,      nullptr,    10}
		,{  "KeepExtraVersions",                 &keepnver,         nullptr,    10}
		,{  "UseWrap",                           &usewrap,          nullptr,    10}
		,{  "FreshIndexMaxAge",                  &maxtempdelay,     nullptr,    10}
		,{  "RedirMax",                          &redirmax,         nullptr,    10}
		,{  "VfileUseRangeOps",                  &vrangeops,        nullptr,    10}
		,{  "ResponseFreezeDetectTime",          &stucksecs,        nullptr,    10}
		,{  "ReuseConnections",                  &persistoutgoing,  nullptr,    10}
		,{  "PipelineDepth",                     &pipelinelen,      nullptr,    10}
		,{  "ExSuppressAdminNotification",       &exsupcount,       nullptr,    10}
		,{  "OptProxyTimeout",                   &optproxytimeout,  nullptr,    10}
		,{  "MaxDlSpeed",                        &maxdlspeed,       nullptr,    10}
		,{  "MaxInresponsiveDlSize",             &maxredlsize,      nullptr,    10}
		,{  "OptProxyCheckInterval",             &optProxyCheckInt, nullptr,    10}
		// octal base interpretation of UNIX file permissions
		,{  "DirPerms",                          &dirperms,         nullptr,    8}
		,{  "FilePerms",                         &fileperms,        nullptr,    8}

		,{ "Verbose", 			nullptr,		"Option is deprecated, ignoring the value." , 10}
		,{ "MaxSpareThreadSets",&tpstandbymax, 	"Deprecated option name, mapped to MaxStandbyConThreads", 10}
		,{ "OldIndexUpdater",	&oldupdate, 	"Option is deprecated, ignoring the value." , 10}
		,{ "Patrace",	&patrace, 				"Don't use in config files!" , 10}
		,{ "NoSSLchecks",	&nsafriendly, 		"Disable SSL security checks" , 10}
};


tProperty n2pTbl[] =
{
{ "Proxy", [](cmstring& key, cmstring& value)
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
{ "LocalDirs", [](cmstring& key, cmstring& value) -> bool
{
	if(g_bNoComplex)
	return true;
	_ParseLocalDirs(value);
	return !localdirs.empty();
}, [](bool) -> string
{
	string ret;
	for(auto kv : localdirs)
	ret += kv.first + " " + kv.second + "; ";
	return ret;
} },
{ "Remap-", [](cmstring& key, cmstring& value) -> bool
{
	if(g_bNoComplex)
	return true;

	string vname=key.substr(6, key.npos);
	if(vname.empty())
	{
		if(!g_bQuiet)
		cerr << "Bad repository name in " << key << endl;
		return false;
	}
	int type(-1); // nothing =-1; prefixes =0 ; backends =1; flags =2
		for(tSplitWalk split(&value); split.Next();)
		{
			cmstring s(split);
			if(s.empty())
			continue;
			if(s.at(0)=='#')
			break;
			if(type<0)
			type=0;
			if(s.at(0)==';')
			++type;
			else if(0 == type)
			AddRemapInfo(false, s, vname);
			else if(1 == type)
			AddRemapInfo(true, s, vname);
			else if(2 == type)
			AddRemapFlag(s, vname);
		}
		if(type<0)
		{
			if(!g_bQuiet)
			cerr << "Invalid entry, no configuration: " << key << ": " << value <<endl;
			return false;
		}
		_AddHooksFile(vname);
		return true;
	}, [](bool) -> string
	{
		return "# mixed options";
	} },
{ "AllowUserPorts", [](cmstring& key, cmstring& value) -> bool
{
	if(!pUserPorts)
	pUserPorts=new bitset<TCP_PORT_MAX>;
	for(tSplitWalk split(&value); split.Next();)
	{
		cmstring s(split);
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
{ "ConnectProto", [](cmstring& key, cmstring& value) -> bool
{
	int *p = conprotos;
	for (tSplitWalk split(&value); split.Next(); ++p)
	{
		cmstring val(split);
		if (val.empty())
		break;

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
{ "AdminAuth", [](cmstring& key, cmstring& value) -> bool
{
	adminauth=value;
	adminauthB64=EncodeBase64Auth(value);
	return true;
}, [](bool) -> string
{
	return "#"; // TOP SECRET";
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

string * GetStringPtr(LPCSTR key) {
	for(auto &ent : n2sTbl)
		if(0==strcasecmp(key, ent.name))
			return ent.ptr;
	return nullptr;
}

int * GetIntPtr(LPCSTR key, int &base) {
	for(auto &ent : n2iTbl)
	{
		if(0==strcasecmp(key, ent.name))
		{
			if(ent.warn)
				cerr << "Warning, " << key << ": " << ent.warn << endl;
			base = ent.base;
			return ent.ptr;
		}
	}
	return nullptr;
}

tProperty* GetPropPtr(cmstring& key)
{
	auto sep = key.find('-');
	auto szkey = key.c_str();
	for (auto &ent : n2pTbl)
	{
		if (0 == strcasecmp(szkey, ent.name))
			return &ent;
		// identified as prefix, with matching length?
		if(sep != stmiss && 0==strncasecmp(szkey, ent.name, sep) && 0 == ent.name[sep+1])
			return &ent;
	}
	return nullptr;
}

int * GetIntPtr(LPCSTR key) {
	for(auto &ent : n2iTbl)
		if(0==strcasecmp(key, ent.name))
			return ent.ptr;
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
	inline operator bool() const { return reader.CheckGoodState(false, &sFilename); }
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
	for(acfg::tCfgIter itor(file); itor.Next();)
		if(StrHas(itor.sLine, needle))
			return true;
	return false;
}

inline void _FixPostPreSlashes(string &val)
{
	// fix broken entries

	if (val.empty() || val.at(val.length()-1) != '/')
		val.append("/");
	if (val.at(0) != '/')
		val.insert(0, "/", 1);
}

bool ReadOneConfFile(const string & szFilename, bool bReadErrorIsFatal=true)
{
	tCfgIter itor(szFilename);
	itor.reader.CheckGoodState(bReadErrorIsFatal, &szFilename);

	NoCaseStringMap dupeCheck;

	while(itor.Next())
	{
#ifdef DEBUG
		cerr << itor.sLine <<endl;
#endif
		// XXX: To something about escaped/quoted version
		tStrPos pos=itor.sLine.find('#');
		if(stmiss != pos)
			itor.sLine.erase(pos);

		if(! SetOption(itor.sLine, &dupeCheck))
			BARF("Error reading main options, terminating.");
	}
	return true;
}

inline decltype(repoparms)::iterator GetRepoEntryRef(const string & sRepName)
{
	auto it = repoparms.find(sRepName);
	if(repoparms.end() != it)
		return it;
	// strange...
	auto rv(repoparms.insert(make_pair(sRepName,tRepoData())));
	return rv.first;
}

inline bool ParseOptionLine(const string &sLine, string &key, string &val)
{
	string::size_type posCol = sLine.find(":");
	string::size_type posEq = sLine.find("=");
	if (posEq==stmiss && posCol==stmiss)
	{
		if(!g_bQuiet)
			cerr << "Not a valid configuration directive: " << sLine <<endl;
		return false;
	}
	string::size_type pos;
	if (posEq!=stmiss && posCol!=stmiss)
		pos=min(posEq,posCol);
	else if (posEq!=stmiss)
		pos=posEq;
	else
		pos=posCol;

	key=sLine.substr(0, pos);
	val=sLine.substr(pos+1);
	trimString(key);
	trimString(val);
	if(key.empty())
		return false; // weird

	if(endsWithSzAr(val, "\\"))
		cerr << "Warning: multilines are not supported, consider using \\n." <<endl;

	return true;
}


inline void AddRemapFlag(const string & token, const string &repname)
{
	mstring key, value;
	if(!ParseOptionLine(token, key, value))
		return;

	tRepoData &where = repoparms[repname];

	if(key=="keyfile")
	{
		if(value.empty())
			return;
		if (acfg::debug&LOG_FLUSH)
			cerr << "Fatal keyfile for " <<repname<<": "<<value <<endl;

		where.m_keyfiles.emplace_back(value);
	}
	else if(key=="deltasrc")
	{
		if(value.empty())
			return;

		if(!endsWithSzAr(value, "/"))
			value+="/";

		if(!where.m_deltasrc.SetHttpUrl(value))
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

tStrDeq ExpandFileTokens(cmstring &token)
{
	string sPath = token.substr(5);
	if (sPath.empty())
		BARF("Bad file spec for repname, file:?");
	bool bAbs = IsAbsolute(sPath);
	if (suppdir.empty() || bAbs)
	{
		if (!bAbs)
			sPath = confdir + sPathSep + sPath;
		return ExpandFilePattern(sPath, true);
	}
	auto pat = confdir + sPathSep + sPath;
	StrSubst(pat, "//", "/");
	auto res = ExpandFilePattern(pat, true);
	if (res.size() == 1 && !Cstat(res.front()))
		res.clear(); // not existing, wildcard returned
	pat = suppdir + sPathSep + sPath;
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

inline void AddRemapInfo(bool bAsBackend, const string & token,
		const string &repname)
{
	if (0!=token.compare(0, 5, "file:"))
	{
		tHttpUrl url;
		if(! url.SetHttpUrl(token))
			BARF(token + " <-- bad URL detected");
		_FixPostPreSlashes(url.sPath);

		if (bAsBackend)
			repoparms[repname].m_backends.emplace_back(url);
		else
			mapUrl2pVname[url.sHost+":"+url.GetPort()].emplace_back(
					url.sPath, GetRepoEntryRef(repname));
	}
	else
	{
		auto func = bAsBackend ? ReadBackendsFile : ReadRewriteFile;
		unsigned count = 0;
		for(auto& src : ExpandFileTokens(token))
			count += func(src, repname);
		if(!count)
			for(auto& src : ExpandFileTokens(token + ".default"))
				count = func(src, repname);
		if(!count && !g_bQuiet)
			cerr << "WARNING: No configuration was read from " << token << endl;
	}
}

struct tHookHandler: public tRepoData::IHookHandler, public base_with_mutex
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
			g_victor.ScheduleFor(downTimeNext, cleaner::TYPE_ACFGHOOKS);
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
				aclog::err(tSS() << "Warning: " << cmdCon << " returned with error code.");
		}
	}
};

inline void _AddHooksFile(cmstring& vname)
{
	tCfgIter itor(acfg::confdir+"/"+vname+".hooks");
	if(!itor)
		return;

	struct tHookHandler &hs = *(new tHookHandler(vname));
	mstring key,val;
	while (itor.Next())
	{
		if(!ParseOptionLine(itor.sLine, key, val))
			continue;

		const char *p = key.c_str();
		if (strcasecmp("PreUp", p) == 0)
		{
			hs.cmdCon = val;
		}
		else if (strcasecmp("Down", p) == 0)
		{
			hs.cmdRel = val;
		}
		else if (strcasecmp("DownTimeout", p) == 0)
		{
			errno = 0;
			unsigned n = strtoul(val.c_str(), nullptr, 10);
			if (!errno)
				hs.downDuration = n;
		}
	}
	repoparms[vname].m_pHooks = &hs;
}

inline void _ParseLocalDirs(cmstring &value)
{
	for(tSplitWalk splitter(&value, ";"); splitter.Next(); )
	{
		mstring token=splitter.str();
		trimString(token);
		tStrPos pos = token.find_first_of(SPACECHARS);
		if(stmiss == pos)
		{
			cerr << "Cannot map " << token << ", needed format: virtualdir realdir, ignoring it";
			continue;
		}
		string from(token, 0, pos);
		trimString(from, "/");
		string what(token, pos);
		trimString(what, SPACECHARS "'\"");
		if(what.empty())
		{
			cerr << "Unsupported target of " << from << ": " << what << ", ignoring it" << endl;
			continue;
		}
		localdirs[from]=what;
	}
}


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

				tSplitWalk split(&itor.sLine);
				if (!split.Next())
					continue;

				mstring mimetype = split;
				if (startsWithSz(mimetype, "#"))
					continue;

				while (split.Next())
				{
					mstring suf = split;
					if (startsWithSz(suf, "#"))
						break;
					mimemap[suf] = mimetype;
				}
			}
		}
	}

	tStrPos dpos = path.find_last_of('.');
	if (dpos != stmiss)
	{
		NoCaseStringMap::const_iterator it = acfg::mimemap.find(path.substr(
				dpos + 1));
		if (it != acfg::mimemap.end())
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

bool SetOption(const string &sLine, NoCaseStringMap *pDupeCheck)
{
	string key, value;

	if(!ParseOptionLine(sLine, key, value))
		return false;

	string * psTarget;
	int * pnTarget;
	tProperty * ppTarget;
	int nNumBase(10);

	if ( nullptr != (psTarget = GetStringPtr(key.c_str())))
	{

		if(pDupeCheck && !g_bQuiet)
		{
			mstring &w = (*pDupeCheck)[key];
			if(w.empty())
				w = value;
			else
				cerr << "WARNING: " << key << " was previously set to " << w << endl;
		}

		*psTarget=value;
	}
	else if ( nullptr != (pnTarget = GetIntPtr(key.c_str(), nNumBase)))
	{

		if(pDupeCheck && !g_bQuiet)
		{
			mstring &w = (*pDupeCheck)[key];
			if(w.empty())
				w = value;
			else
				cerr << "WARNING: " << key << " was already set to " << w << endl;
		}

		const char *pStart=value.c_str();
		if(! *pStart)
		{
			cerr << "Missing value for " << key << " option!" <<endl;
			return false;
		}
		
		errno=0;
		char *pEnd(nullptr);
		long nVal = strtol(pStart, &pEnd, nNumBase);

		if(RESERVED_DEFVAL == nVal)
		{
			cerr << "Bad value for " << key << " (protected value, use another one)" <<endl;
			return false;
		}

		*pnTarget=nVal;

		if (errno)
		{
			cerr << "Invalid number for " << key << " ";
			perror("option");
			return false;
		}
		if(*pEnd)
		{
			cerr << "Bad value for " << key << " option or found trailing garbage: " << pEnd <<endl;
			return false;
		}
	}
	else if ( nullptr != (ppTarget = GetPropPtr(key)))
	{
		return ppTarget->set(key, value);
	}
	else
	{
		if(!g_bQuiet)
			cerr << "Warning, unknown configuration directive: " << key <<endl;
		return false;
	}
	return true;
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

unsigned ReadBackendsFile(const string & sFile, const string &sRepName)
{
	unsigned nAddCount=0;
	string key, val;
	tHttpUrl entry;

	tCfgIter itor(sFile);
	if(debug&6)
		cerr << "Reading backend file: " << sFile <<endl;

	if(!itor)
	{
		if(debug&6)
			cerr << "No backend data found, file ignored."<<endl;
		return 0;
	}
	
	while(itor.Next())
	{
		if(debug & LOG_DEBUG)
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

void ShutDown()
{
	mapUrl2pVname.clear();
	repoparms.clear();
}

/* This parses also legacy files, i.e. raw RFC-822 formated mirror catalogue from the
 * Debian archive maintenance repository.
 */
unsigned ReadRewriteFile(const string & sFile, cmstring& sRepName)
{
	unsigned nAddCount=0;
	filereader reader;
	if(debug>4)
		cerr << "Reading rewrite file: " << sFile <<endl;
	reader.OpenFile(sFile, false, 1);
	reader.CheckGoodState(true, &sFile);

	tStrVec hosts, paths;
	string sLine, key, val;
	tHttpUrl url;

	while (reader.GetOneLine(sLine))
	{
		trimFront(sLine);

		if (0 == sLine.compare(0, 1, "#"))
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


tRepoData::~tRepoData()
{
	delete m_pHooks;
}

void ReadConfigDirectory(const char *szPath, bool bReadErrorIsFatal)
{
	dump_proc_status();
	char buf[PATH_MAX];
	if(!realpath(szPath, buf))
		BARF("Failed to open config directory");

	confdir=buf; // pickup the last config directory

#if defined(HAVE_WORDEXP) || defined(HAVE_GLOB)
	for(const auto& src: ExpandFilePattern(confdir+SZPATHSEP "*.conf", true))
		ReadOneConfFile(src, bReadErrorIsFatal);
#else
	ReadOneConfFile(confdir+SZPATHSEP"acng.conf", bReadErrorIsFatal);
#endif
	dump_proc_status();
	if(debug & LOG_DEBUG)
	{
		unsigned nUrls=0;
		for(const auto& x: mapUrl2pVname)
			nUrls+=x.second.size();

		cerr << "Loaded " << repoparms.size() << " backend descriptors\nLoaded mappings for "
				<< mapUrl2pVname.size() << " hosts and " << nUrls<<" paths\n";
	}
}

void PostProcConfig()
{
	mapUrl2pVname.rehash(mapUrl2pVname.size());
	
	if(port.empty()) // heh?
		port=ACNG_DEF_PORT;

	if(connectPermPattern == "~~~")
	   connectPermPattern="^bugs.debian.org:443$";

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
	   BARF("Cache directory unknown or not absolute, terminating...");
   
   if(!rechecks::CompileExpressions())
	   BARF("An error occurred while compiling file type regular expression!");
   
   if(acfg::tpthreadmax < 0)
	   acfg::tpthreadmax = MAX_VAL(int);
	   
   // get rid of duplicated and trailing slash(es)
	for(tStrPos pos; stmiss != (pos = cachedir.find(SZPATHSEP SZPATHSEP )); )
		cachedir.erase(pos, 1);

	cacheDirSlash=cachedir+CPATHSEP;

   if(!pidfile.empty() && pidfile.at(0) != CPATHSEP)
	   BARF("Pid file path must be absolute, terminating...");
   
   if(!acfg::agentname.empty())
	   acfg::agentheader=string("User-Agent: ")+acfg::agentname + "\r\n";

   stripPrefixChars(acfg::reportpage, '/');

   trimString(acfg::requestapx);
   if(!acfg::requestapx.empty())
	   acfg::requestapx = unEscape(acfg::requestapx);

   // create working paths before something else fails somewhere
   if(!fifopath.empty())
	   mkbasedir(acfg::fifopath);
   if(!cachedir.empty())
	   mkbasedir(acfg::cachedir);
   if(! pidfile.empty())
	   mkbasedir(acfg::pidfile);

   if(nettimeout < 5) {
	   cerr << "Warning, NetworkTimeout value too small, using: 5." << endl;
	   nettimeout = 5;
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

   if(!rechecks::CompileUncExpressions(rechecks::NOCACHE_REQ,
		   tmpDontcacheReq.empty() ? tmpDontcache : tmpDontcacheReq)
   || !rechecks::CompileUncExpressions(rechecks::NOCACHE_TGT,
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
	   redirmax = forcemanaged ? 0 : ACFG_REDIRMAX_DEFAULT;

   if(!persistoutgoing)
	   pipelinelen = 1;

   if(!pipelinelen) {
	   cerr << "Warning, remote pipeline depth of 0 makes no sense, assuming 1." << endl;
	   pipelinelen = 1;
   }

} // PostProcConfig

void dump_config(bool includeDelicate)
{
	ostream &cmine(cout);

	for (auto& n2s : n2sTbl)
		if (n2s.ptr)
			cmine << n2s.name << " = " << *n2s.ptr << endl;

	if (acfg::debug >= LOG_DEBUG)
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
		if (n2i.ptr)
			cmine << n2i.name << " = " << *n2i.ptr << endl;

	for (const auto& x : n2pTbl)
	{
		auto val(x.get(includeDelicate));
		if(startsWithSz(val, "#")) continue;
		cmine << x.name << " = " << val << endl;
	}

#ifndef DEBUG
	if (acfg::debug >= LOG_DEBUG)
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
		tHookHandler & hooks = *(static_cast<tHookHandler*> (parm.second.m_pHooks));
		lockguard g(hooks);
		if (hooks.downTimeNext)
		{
			if (hooks.downTimeNext <= now) // time to execute
			{
				if(acfg::debug&LOG_MORE)
					aclog::misc(hooks.cmdRel, 'X');
				if(acfg::debug & LOG_FLUSH)
					aclog::flush();

				if(system(hooks.cmdRel.c_str()))
					aclog::err(tSS() << "Warning: " << hooks.cmdRel << " returned with error code.");
				hooks.downTimeNext = 0;
			}
			else // in future, use the soonest time
				ret = min(ret, hooks.downTimeNext);
		}
	}
	return ret;
}

acmutex authLock;

int CheckAdminAuth(LPCSTR auth)
{
	if(acfg::adminauthB64.empty())
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

#endif // MINIBUILD

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

} // namespace acfg

namespace rechecks
{
	// this has the exact order of the "regular" types in the enum
	struct { regex_t *pat=nullptr, *extra=nullptr; } rex[ematchtype_max];
	vector<regex_t> vecReqPatters, vecTgtPatterns;

bool CompileExpressions()
{
	auto compat = [](regex_t* &re, LPCSTR ps)
	{
		if(!ps ||! *ps )
		return true;
		re=new regex_t;
		int nErr=regcomp(re, ps, REG_EXTENDED);
		if(!nErr)
		return true;

		char buf[1024];
		regerror(nErr, re, buf, sizeof(buf));
		delete re;
		re=nullptr;
		buf[_countof(buf)-1]=0; // better be safe...
			std::cerr << buf << ": " << ps << std::endl;
			return false;
		};
	using namespace acfg;
	return (compat(rex[FILE_SOLID].pat, pfilepat.c_str())
			&& compat(rex[FILE_VOLATILE].pat, vfilepat.c_str())
			&& compat(rex[FILE_WHITELIST].pat, wfilepat.c_str())
			&& compat(rex[FILE_SOLID].extra, pfilepatEx.c_str())
			&& compat(rex[FILE_VOLATILE].extra, vfilepatEx.c_str())
			&& compat(rex[FILE_WHITELIST].extra, wfilepatEx.c_str())
			&& compat(rex[NASTY_PATH].pat, BADSTUFF_PATTERN)
			&& compat(rex[FILE_SPECIAL_SOLID].pat, spfilepat.c_str())
			&& compat(rex[FILE_SPECIAL_SOLID].extra, spfilepatEx.c_str())
			&& compat(rex[FILE_SPECIAL_VOLATILE].pat, svfilepat.c_str())
			&& compat(rex[FILE_SPECIAL_VOLATILE].extra, svfilepatEx.c_str())
			&& (connectPermPattern == "~~~" ?
			true : compat(rex[PASSTHROUGH].pat, connectPermPattern.c_str())));
}

// match the specified type by internal pattern PLUS the user-added pattern
inline bool MatchType(cmstring &in, eMatchType type)
{
	if(rex[type].pat && !regexec(rex[type].pat, in.c_str(), 0, nullptr, 0))
		return true;
	if(rex[type].extra && !regexec(rex[type].extra, in.c_str(), 0, nullptr, 0))
		return true;
	return false;
}

bool Match(cmstring &in, eMatchType type)
{
	if(MatchType(in, type))
		return true;
	// very special behavior... for convenience
	return (type == FILE_SOLID && MatchType(in, FILE_SPECIAL_SOLID))
		|| (type == FILE_VOLATILE && MatchType(in, FILE_SPECIAL_VOLATILE));
}

eMatchType GetFiletype(const string & in)
{
	if (MatchType(in, FILE_SPECIAL_VOLATILE))
		return FILE_VOLATILE;
	if (MatchType(in, FILE_SPECIAL_SOLID))
		return FILE_SOLID;
	if (MatchType(in, FILE_VOLATILE))
		return FILE_VOLATILE;
	if (MatchType(in, FILE_SOLID))
		return FILE_SOLID;
	return FILE_INVALID;
}

#ifndef MINIBUILD

inline bool CompileUncachedRex(const string & token, NOCACHE_PATTYPE type, bool bRecursiveCall)
{
	auto & patvec = (NOCACHE_TGT == type) ? vecTgtPatterns : vecReqPatters;

	if (0!=token.compare(0, 5, "file:")) // pure pattern
	{
		unsigned pos = patvec.size();
		patvec.resize(pos+1);
		return 0==regcomp(&patvec[pos], token.c_str(), REG_EXTENDED);
	}
	else if(!bRecursiveCall) // don't go further than one level
	{
		tStrDeq srcs = acfg::ExpandFileTokens(token);
		for(const auto& src: srcs)
		{
			acfg::tCfgIter itor(src);
			if(!itor)
			{
				cerr << "Error opening pattern file: " << src <<endl;
				return false;
			}
			while(itor.Next())
			{
				if(!CompileUncachedRex(itor.sLine, type, true))
					return false;
			}
		}

		return true;
	}

	cerr << token << " is not supported here" <<endl;
	return false;
}


bool CompileUncExpressions(NOCACHE_PATTYPE type, cmstring& pat)
{
	for(tSplitWalk split(&pat); split.Next(); )
		if (!CompileUncachedRex(split, type, false))
			return false;
	return true;
}

bool MatchUncacheable(const string & in, NOCACHE_PATTYPE type)
{
	for(const auto& patre: (type == NOCACHE_REQ) ? vecReqPatters : vecTgtPatterns)
		if(!regexec(&patre, in.c_str(), 0, nullptr, 0))
			return true;
	return false;
}

#endif //MINIBUILD


} // namespace rechecks


#ifndef MINIBUILD
LPCSTR ReTest(LPCSTR s)
{
	static LPCSTR names[rechecks::ematchtype_max] =
	{
				"FILE_SOLID", "FILE_VOLATILE",
				"FILE_WHITELIST",
				"NASTY_PATH", "PASSTHROUGH",
				"FILE_SPECIAL_SOLID"
	};
	auto t = rechecks::GetFiletype(s);
	if(t<0 || t>=rechecks::ematchtype_max)
		return "NOMATCH";
	return names[t];
}
#endif
