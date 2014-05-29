
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

using namespace MYSTD;


#define _iterPos(it, start) (it-start.begin())/sizeof(it)
#define sProblemLoc szPath<< ':'<< _iterPos(it, lines)

// hint to use the main configuration excluding the complex directives
bool g_testMode=false;

#define BARF(x) {if(!g_testMode){ cerr << x << endl; exit(EXIT_FAILURE); }}
#define BADSTUFF_PATTERN "\\.\\.($|%|/)"
mstring PTHOSTS_PATTERN;

namespace rechecks
{
bool CompileExpressions();
bool CompileUncExpressions(const string & req, const string & tgt);
}


namespace acfg {


// internal stuff:
string sPopularPath("/debian/");

string tmpDontcache, tmpDontcacherq, tmpDontcacheResolved;

tStrMap localdirs;
static class : public lockable, public NoCaseStringMap {} mimemap;

MYSTD::bitset<65536> *pUserPorts = NULL;

typedef struct
{
	const char *name; mstring *ptr;
	const char *warn;
}
MapNameToString;

typedef struct
{
	const char *name; int *ptr;
	const char *warn; int base;
}
MapNameToInt;

#ifndef MINIBUILD

MapNameToString n2sTbl[] = {
		{  "Port", 			&port , 0}
		,{ "CacheDir", 	&cachedir, 0 }
		,{ "LogDir", 	&logdir , 0}
		,{ "SupportDir", 	&suppdir, 0}
		,{ "SocketPath", 	&fifopath, 0}
		,{ "PidFile", 	&pidfile, 0}
		,{ "ReportPage",&reportpage, 0}
		,{ "VfilePattern", &vfilepat, 0}
		,{ "PfilePattern", &pfilepat, 0}
		,{ "WfilePattern", &wfilepat, 0}
		,{ "AdminAuth",  &adminauth, 0}
		,{ "BindAddress", &bindaddr, 0}
		,{ "UserAgent", &agentname, 0}
		,{ "DontCache",	&tmpDontcache, 0}
		,{ "DontCacheRequested",	&tmpDontcacherq, 0}
		,{ "DontCacheResolved",	&tmpDontcacheResolved, 0}
		,{ "PrecacheFor", &mirrorsrcs, 0}
		,{ "RequestAppendix", &requestapx, 0}
		,{ "PassThroughPattern", &PTHOSTS_PATTERN, 0}
		,{ "CApath", &capath, 0}
		,{ "CAfile", &cafile, 0}
};

MapNameToInt n2iTbl[] = {
		{ "Debug", 		&debug, 0 , 10}
		,{ "OfflineMode", 	&offlinemode , NULL, 10}
		,{ "ForeGround", 	&foreground , NULL, 10}
		,{ "Verbose", 		NULL, "Option is deprecated, ignoring the value." , 10}
		,{ "ForceManaged", 	&forcemanaged , NULL, 10}
		,{ "StupidFs", 		&stupidfs , NULL, 10}
		,{ "VerboseLog",	&verboselog , NULL, 10}
		,{ "ExTreshold",	&extreshhold, NULL, 10}
		,{ "MaxSpareThreadSets",	&tpstandbymax, "Deprecated option name, mapped to MaxStandbyConThreads", 10}
		,{ "MaxStandbyConThreads",	&tpstandbymax, NULL, 10}
		,{ "MaxConThreads",	&tpthreadmax, NULL, 10}
		,{ "DnsCacheSeconds", &dnscachetime, NULL, 10}
		,{ "UnbufferLogs", &debug , NULL, 10}
		,{ "ExAbortOnProblems", &exfailabort, NULL, 10}
		,{ "ExposeOrigin", &exporigin, NULL, 10}
		,{ "LogSubmittedOrigin", &logxff, NULL, 10}
		,{ "OldIndexUpdater", &oldupdate, "Option is deprecated, ignoring the value." , 10}
		,{ "RecompBz2", &recompbz2, NULL, 10}
		,{ "NetworkTimeout", &nettimeout, NULL, 10}
		,{ "MinUpdateInterval", &updinterval, NULL, 10}
		,{ "ForwardBtsSoap", &forwardsoap, NULL, 10}
		,{ "KeepExtraVersions", &keepnver, NULL, 10}
		,{ "UseWrap", &usewrap, NULL, 10}
		,{ "FreshIndexMaxAge", &maxtempdelay, NULL, 10}
		,{ "RedirMax", &redirmax, NULL, 10}
		,{ "VfileUseRangeOps", &vrangeops, NULL, 10}
		,{ "ResponseFreezeDetectTime", &stucksecs, NULL, 10}
		,{ "ReuseConnections", &persistoutgoing, NULL, 10}
		,{ "PipelineDepth", &pipelinelen, NULL, 10}
		,{ "ExSuppressAdminNotification", &exsupcount, NULL, 10}
		,{ "OptProxyTimeout", &optproxytimeout, NULL, 10}

		,{ "DirPerms", &dirperms, NULL, 8}
		,{ "FilePerms", &fileperms, NULL, 8}
};

void ReadRewriteFile(const string & sFile, const string & sRepName);
void ReadBackendsFile(const string & sFile, const string &sRepName);

map<cmstring, tRepoData> repoparms;

// maps hostname:port -> { <path,pointer>, ... }
unordered_map<string, list<pair<cmstring,decltype(repoparms)::iterator>>> mapUrl2pVname;


string * GetStringPtr(const string &key) {
	for(auto &ent : n2sTbl)
	{
		if(0==strcasecmp(key.c_str(), ent.name))
		{
			if(ent.warn)
				cerr << "Warning, " << key << ": " << ent.warn << endl;
			return ent.ptr;
		}
	}
	return NULL;
}

int * GetIntPtr(const string &key, int &base) {
	for(auto &ent : n2iTbl)
	{
		if(0==strcasecmp(key.c_str(), ent.name))
		{
			if(ent.warn)
				cerr << "Warning, " << key << ": " << ent.warn << endl;
			base = ent.base;
			return ent.ptr;
		}
	}
	return NULL;
}

void printVar(cmstring &key)
{
	string *ps=GetStringPtr(key);
	if(ps)
		cout << *ps << endl;
	int base, *pn=GetIntPtr(key, base);
	if(pn)
		cout << *pn <<endl;
}

// shortcut for frequently needed code, opens the config file, reads step-by-step
// and skips comment and empty lines
struct tCfgIter
{
	filereader reader;
	string sLine;
	tCfgIter(cmstring &fn)
	{
		reader.OpenFile(fn);
		reader.AddEofLines();
	}
	inline operator bool() const { return reader.CheckGoodState(false); }
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
		if(itor.sLine.find(needle) != stmiss)
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

bool ReadOneConfFile(const string & szFilename)
{
	tCfgIter itor(szFilename);
	itor.reader.CheckGoodState(true);

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

		if(! SetOption(itor.sLine, g_testMode, &dupeCheck))
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

inline bool ParseOptionLine(const string &sLine, string &key, string &val, bool bQuiet)
{
	string::size_type posCol = sLine.find(":");
	string::size_type posEq = sLine.find("=");
	if (posEq==stmiss && posCol==stmiss)
	{
		if(!bQuiet)
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

	return true;
}


inline void AddRemapFlag(const string & token, const string &repname)
{
	mstring key, value;
	if(!ParseOptionLine(token, key, value, true))
		return;

	tRepoData &where = repoparms[repname];

	if(key=="keyfile")
	{
		if(value.empty())
			return;
		if (acfg::debug&LOG_FLUSH)
			cerr << "Fatal keyfile for " <<repname<<": "<<value <<endl;

		where.m_keyfiles.push_back(value);
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
		static MYSTD::list<tHttpUrl> alt_proxies;
		tHttpUrl cand;
		if(value.empty() || cand.SetHttpUrl(value))
		{
			alt_proxies.push_back(cand);
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
	tStrDeq srcs;
	string sPath = token.substr(5);
	if (sPath.empty())
		BARF("Bad file spec for repname, file:?");
	bool bAbs = IsAbsolute(sPath);
	if (suppdir.empty() || bAbs)
	{
		if (!bAbs)
			sPath = confdir + sPathSep + sPath;
		srcs = ExpandFilePattern(sPath, true);
	}
	else
	{
		tStrMap bname2path;
		for (const auto& dir : { &suppdir, &confdir })
		{
			// chop slashes. That should not be required but better be sure.
			while(endsWithSzAr(*dir, sPathSep) && dir->size()>1)
				dir->resize(dir->size()-1);

			// temporary only
			srcs = ExpandFilePattern( (cmstring) *dir + sPathSep + sPath, true);
			if (srcs.size() == 1 && GetFileSize(srcs[0], -23) == off_t(-23))
				continue; // file not existing, wildcard returned

			// hits in confdir will overwrite those from suppdir
			for (; !srcs.empty(); srcs.pop_back())
				bname2path[::GetBaseName(srcs.back())] = srcs.back();
		}
		if (bname2path.empty())
		{
			cerr << "WARNING: No URL list file matching " << token
					<< " found in config or support directories." << endl;
		}
		srcs.clear();
		for (tStrMap::const_iterator it = bname2path.begin(); it != bname2path.end(); ++it)
			srcs.push_back(it->second);
	}
	return srcs;
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
			repoparms[repname].m_backends.push_back(url);
		else
			mapUrl2pVname[url.sHost+":"+url.GetPort()].push_back(
					make_pair(url.sPath, GetRepoEntryRef(repname)));
	}
	else
	{
		tStrDeq srcs = ExpandFileTokens(token);
		
		for(tStrDeq::const_iterator it=srcs.begin(); it!=srcs.end(); it++)
		{
			if (bAsBackend)
				ReadBackendsFile(*it, repname);
			else
				ReadRewriteFile(*it, repname);
		}
	}
}

struct tHookHandler: public tRepoData::IHookHandler, public lockable
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
	virtual void JobRelease()
	{
		setLockGuard;
		if (0 >= --m_nRefCnt)
		{
			//system(cmdRel.c_str());
			downTimeNext = ::time(0) + downDuration;
			g_victor.ScheduleFor(downTimeNext, cleaner::TYPE_ACFGHOOKS);
		}
	}
	virtual void JobConnect()
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

void _AddHooksFile(cmstring& vname)
{
	tCfgIter itor(acfg::confdir+"/"+vname+".hooks");
	if(!itor)
		return;

	struct tHookHandler &hs = *(new tHookHandler(vname));
	mstring key,val;
	while (itor.Next())
	{
		if(!ParseOptionLine(itor.sLine, key, val, false))
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
			uint n = strtoul(val.c_str(), NULL, 10);
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
	static cmstring os("application/octet-stream"), tp("text/plain"), nix("");
	filereader f;
	if(f.OpenFile(path, true))
	{
		size_t maxLen = MYSTD::min(size_t(255), f.GetSize());
		for(UINT i=0; i< maxLen; ++i)
		{
			if(!isascii((UINT) *(f.GetBuffer()+i)))
				return os;
		}
		return tp;
	}
	return nix;
}

bool SetOption(const string &sLine, bool bQuiet, NoCaseStringMap *pDupeCheck)
{
	string key, value;

	if(!ParseOptionLine(sLine, key, value, bQuiet))
		return false;

	string * psTarget;
	int * pnTarget;
	int nNumBase(10);

	if ( NULL != (psTarget = GetStringPtr(key)))
	{

		if(pDupeCheck && !bQuiet)
		{
			mstring &w = (*pDupeCheck)[key];
			if(w.empty())
				w = value;
			else
				cerr << "WARNING: " << key << " was previously set to " << w << endl;
		}

		*psTarget=value;
	}
	else if ( NULL != (pnTarget = GetIntPtr(key, nNumBase)))
	{

		if(pDupeCheck && !bQuiet)
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
		char *pEnd(0);
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

	else if(0==strcasecmp(key.c_str(), "Proxy"))
	{
		if(value.empty())
			proxy_info=tHttpUrl();
		else
		{
			if (!proxy_info.SetHttpUrl(value) || proxy_info.sHost.empty())
				BARF("Invalid proxy specification, aborting...");
		}
	}
	else if(0==strcasecmp(key.c_str(), "LocalDirs") && !g_testMode)
	{
		_ParseLocalDirs(value);
		return !localdirs.empty();
	}
	else if(0==strncasecmp(key.c_str(), "Remap-", 6) && !g_testMode)
	{
		string vname=key.substr(6, key.npos);
		tStrVec tokens;
		Tokenize(value, SPACECHARS, tokens);
		if(tokens.empty() || vname.empty())
		{
			if(!bQuiet)
				cerr << "Found invalid entry, ignoring " << key << ": " << value <<endl;
			return false;
		}
		int type(0); // prefixes ; backends ; flags
		for(tStrVecIterConst it=tokens.begin(); it!=tokens.end(); ++it)
		{
			if(it->empty())
				continue;
			if(it->at(0)=='#')
				break;

			if(it->at(0)==';')
				++type;
			else if(0 == type)
				AddRemapInfo(false, *it, vname);
			else if(1 == type)
				AddRemapInfo(true, *it, vname);
			else if(2 == type)
				AddRemapFlag(*it, vname);
		}
		_AddHooksFile(vname);
	}
	else if(0==strcasecmp(key.c_str(), "AllowUserPorts"))
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
	}
	else if(0==strcasecmp(key.c_str(), "ConnectProto"))
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
	}
	else
	{
		if(!bQuiet)
			cerr << "Warning, unknown configuration directive: " << key <<endl;
		return false;
	}
	return true;
}


//const string * GetVnameForUrl(string path, string::size_type * nMatchLen)
bool GetRepNameAndPathResidual(const tHttpUrl & in, string & sRetPathResidual,
		tBackendDataRef &beRef)
{
	sRetPathResidual.clear();
	
	// get all the URLs matching THE HOSTNAME
	auto rangeIt=mapUrl2pVname.find(in.sHost+":"+in.GetPort());
	if(rangeIt == mapUrl2pVname.end())
		return NULL;
	
	tStrPos bestMatchLen(0);
	decltype(repoparms)::iterator pBestHit = repoparms.end();
		
	// now find the longest directory part which is the suffix of requested URL's path
	for (auto& repo : rangeIt->second)
	{
		// rewrite rule path must be a real prefix
		// it's also surrounded by /, ensured during construction
		const string & prefix=repo.first; // path of the rewrite entry
		tStrPos len=prefix.length();
		if (in.sPath.size() > len && 0==in.sPath.compare(0, len, prefix))
		{
			if (len>bestMatchLen)
			{
				bestMatchLen=len;
				pBestHit=repo.second;
			}
		}
	}
		
	if(pBestHit != repoparms.end())
	{
		sRetPathResidual=in.sPath.substr(bestMatchLen);
		beRef=pBestHit;
		return true;
	}
	return false;
}

const tRepoData * GetBackendVec(cmstring &vname)
{
	auto it=repoparms.find(vname);
	if(it==repoparms.end() || it->second.m_backends.empty())
		return NULL;
	return & it->second;
}

void ReadBackendsFile(const string & sFile, const string &sRepName)
{

	int nAddCount=0;
	string key, val;
	tHttpUrl entry;

	tCfgIter itor(sFile);
	if(debug&6)
		cerr << "Reading backend file: " << sFile <<endl;

	if(!itor)
	{
		if(debug&6)
			cerr << "No backend data found, file ignored."<<endl;
		goto try_read_default;
	}
	
	while(itor.Next())
	{
		if(debug & LOG_DEBUG)
			cerr << "backends, got line: " << itor.sLine <<endl;

		trimBack(itor.sLine);

		if( entry.SetHttpUrl(itor.sLine)
				||	( itor.sLine.empty() && ! entry.sHost.empty() && ! entry.sPath.empty()) )
		{
			_FixPostPreSlashes(entry.sPath);
#ifdef DEBUG
			cerr << "Backend: " << sRepName << " <-- " << entry.ToURI(false) <<endl;
#endif		
			repoparms[sRepName].m_backends.push_back(entry);
			nAddCount++;
			entry.clear();
		}
		else if(ParseKeyValLine(itor.sLine, key, val))
		{
			if(keyEq("Site", key))
				entry.sHost=val;
			/* TODO: not supported yet, maybe add later - push to a vector of hosts and add multiple later
			if(keyEq("Aliases", key))
			{
				val+=" ";
				for(string::size_type posA(0), posB(0);
					posA<val.length();
					posA=posB+1)
				{
					posB=val.find_first_of(" \t\r\n\f\v", posA);
					if(posB!=posA)
						hosts.push_back(val.substr(posA, posB-posA));
				}
			}
			*/
			else if(keyEq("Archive-http", key) || keyEq("X-Archive-http", key))
			{
				entry.sPath=val;
			}
		}
		else
		{
			BARF("Bad backend description, around line " << sFile << ":"
					<< itor.reader.GetCurrentLine());
		}
	}

try_read_default:
	if(nAddCount || endsWithSzAr(sFile, ".default"))
		return;
	if(debug>4)
		cerr << "Trying to read replacement from " << sFile << ".default" <<endl;
	ReadBackendsFile(sFile+".default", sRepName);
}

void ShutDown()
{
	mapUrl2pVname.clear();
	repoparms.clear();
}

void ReadRewriteFile(const string & sFile, const string & sRepName)
{

	filereader reader;
	if(debug>4)
		cerr << "Reading rewrite file: " << sFile <<endl;
	reader.OpenFile(sFile);
	reader.CheckGoodState(true);
	reader.AddEofLines();

	tStrVec hosts, paths;
	string sLine, key, val;
	tHttpUrl url;
	
	while(reader.GetOneLine(sLine))
	{
		trimFront(sLine);

		if (0==sLine.compare(0, 1, "#"))
			continue;

		if (url.SetHttpUrl(sLine))
		{
			_FixPostPreSlashes(url.sPath);

			mapUrl2pVname[url.sHost+":"+url.GetPort()].push_back(
					make_pair(url.sPath, GetRepoEntryRef(sRepName)));
#ifdef DEBUG
						cerr << "Mapping: "<< url.ToURI(false)
						<< " -> "<< sRepName <<endl;
#endif
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
					mapUrl2pVname[url.sHost+":"+url.GetPort()].push_back(
						make_pair(url.sPath, GetRepoEntryRef(sRepName)));

#ifdef DEBUG
						cerr << "Mapping: "<< host << path
						<< " -> "<< sRepName <<endl;
#endif
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
				paths.push_back(sPopularPath);
			else
			{
				_FixPostPreSlashes(val);
				paths.push_back(val);
			}
			continue;
		}
	}
}


tRepoData::~tRepoData()
{
	if(m_pHooks)
	{
		delete m_pHooks;
		m_pHooks=NULL;
	}
}

void ReadConfigDirectory(const char *szPath, bool bTestMode)
{
	dump_proc_status();
	char buf[PATH_MAX];
	g_testMode=bTestMode;
	if(!realpath(szPath, buf))
		BARF("Failed to open config directory");

	confdir=buf; // pickup the last config directory

#if defined(HAVE_WORDEXP) || defined(HAVE_GLOB)
	tStrDeq srcs=ExpandFilePattern(confdir+SZPATHSEP"*.conf", true);
	for(tStrDeq::const_iterator it=srcs.begin(); it!=srcs.end(); it++)
		ReadOneConfFile(*it);
#else
	ReadOneConfFile(confdir+SZPATHSEP"acng.conf");
#endif
	dump_proc_status();
}

void PostProcConfig(bool bDumpConfig)
{
	
	if(port.empty()) // heh?
		port=ACNG_DEF_PORT;

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
	   BARF("An error occured while compiling file type regular expression!");
   
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

   if(!adminauth.empty())
	   adminauth=string("Basic ")+EncodeBase64Auth(adminauth);
   
   // create working paths before something else fails somewhere
   if(!fifopath.empty())
	   mkbasedir(acfg::fifopath);
   if(!cachedir.empty())
	   mkbasedir(acfg::cachedir);
   if(! pidfile.empty())
	   mkbasedir(acfg::pidfile);

   if(nettimeout < 5) {
	   cerr << "Warning, NetworkTimeout too small, assuming 5." << endl;
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

   if(!rechecks::CompileUncExpressions(
		   tmpDontcacherq.empty() ? tmpDontcache : tmpDontcacherq,
		   tmpDontcacheResolved.empty() ? tmpDontcache : tmpDontcacheResolved))
   {
	   BARF("An error occurred while compiling regular expression for non-cached paths!");
   }
   tmpDontcache.clear();
   tmpDontcacheResolved.clear();
   tmpDontcacherq.clear();

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

   if (acfg::debug >= LOG_MORE || bDumpConfig)
	{
	   ostream &cmine(bDumpConfig ? cout : cerr);

		for (auto& n2s: n2sTbl)
			if (n2s.ptr)
				cmine << n2s.name << " = " << *n2s.ptr << endl;

		if (acfg::debug >= LOG_DEBUG)
		{
			cerr << "escaped version:" << endl;
			for (const auto& n2s: n2sTbl)
			{
				if (n2s.ptr)
				{
					cerr << n2s.name << " = ";
					for (const char *p = n2s.ptr->c_str(); *p; p++)
						if ('\\' == *p)
							cmine << "\\\\";
						else
							cmine << *p;
					cmine << endl;
				}
			}
		}

		for (const auto& n2i : n2iTbl)
			if (n2i.ptr)
				cmine << n2i.name << " = " << *n2i.ptr << endl;
	}

#ifndef DEBUG
   if(acfg::debug >= LOG_DEBUG)
	   cerr << "\n\nAdditional debugging information not compiled in." << endl << endl;
#endif
   
#if 0 //def DEBUG
#warning adding hook control pins
   for(tMapString2Hostivec::iterator it = repoparms.begin();
		   it!=repoparms.end() ; ++it)
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
} // PostProcConfig

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

#endif // MINIBUILD

} // namespace acfg

namespace rechecks
{
	MYSTD::pair<regex_t, bool> rex[ematchtype_max];

bool CompileExpressions()
{
	const char *pszExpStrings[] = {
			acfg::pfilepat.c_str(), acfg::vfilepat.c_str(), acfg::wfilepat.c_str(),
			BADSTUFF_PATTERN, PTHOSTS_PATTERN.c_str()
	};
	for(UINT i=0; i<_countof(pszExpStrings); i++)
	{
		if(! pszExpStrings[i] || ! pszExpStrings[i][0] )
			continue;

		int nErr=regcomp(&rex[i].first, pszExpStrings[i], REG_EXTENDED);
		if(nErr)
		{
			char buf[1024];
			regerror(nErr,  &rex[i].first, buf, sizeof(buf));
			buf[_countof(buf)-1]=0; // better be safe...
			MYSTD::cerr << buf << ": " << pszExpStrings[i] << MYSTD::endl;
			return false;
		}
		rex[i].second=true;
	}
	return true;
}

bool Match(cmstring &in, eMatchType type)
{
//	LOGSTARTs("MatchPattern");
//	LOG("type: " << type << in);
	if(!rex[type].second)
		return false;

	bool bRes = !regexec(&rex[type].first, in.c_str(), 0, NULL, 0);
//	LOG("result: " << (bRes ? "true" : "false"));
	return bRes;
}

eMatchType GetFiletype(const string & in) {
	if (Match(in, FILE_VOLATILE))
		return FILE_VOLATILE;
	if (Match(in, FILE_SOLID))
		return FILE_SOLID;
	return FILE_INVALID;
}

deque<regex_t> vecReqPatters, vecTgtPatterns;

#ifndef MINIBUILD
inline bool CompileUncachedRex(const string & token, bool bIsTgtPattern, bool bRecursiveCall)
{
	deque<regex_t> & patvec = bIsTgtPattern ? vecTgtPatterns : vecReqPatters;

	if (0!=token.compare(0, 5, "file:")) // pure pattern
	{
		UINT pos = patvec.size();
		patvec.resize(pos+1);
		return 0==regcomp(&patvec[pos], token.c_str(), REG_EXTENDED);
	}
	else if(!bRecursiveCall) // don't go further than one level
	{
		tStrDeq srcs = acfg::ExpandFileTokens(token);
		for(tStrDeq::const_iterator it=srcs.begin(); it!=srcs.end(); it++)
		{
			acfg::tCfgIter itor(*it);
			if(!itor)
			{
				cerr << "Error opening pattern file: " << *it <<endl;
				return false;
			}
			while(itor.Next())
			{
				if(!CompileUncachedRex(itor.sLine, bIsTgtPattern, true))
					return false;
			}
		}

		return true;
	}

	cerr << token << " is not supported here" <<endl;
	return false;
}


bool CompileUncExpressions(const string & req, const string & resolved)
{
	for(int i=0; i<2; ++i) // req, tgt, stop
		for(tSplitWalk split(i? &req : &resolved); split.Next(); )
			if (!CompileUncachedRex(split, i, false))
				return false;
	return true;
}

bool MatchUncacheableRequest(const string & in)
{
	LOGSTART2s("MatchUncacheableRequest", in << " against " << vecReqPatters.size() << " patterns");
	for(deque<regex_t>::const_iterator it=vecReqPatters.begin();
			it!=vecReqPatters.end(); it++)
	{
		if(!regexec(& (*it), in.c_str(), 0, NULL, 0))
			return true;
	}
	return false;
}

bool MatchUncacheableTarget(const string &in)
{
	for(deque<regex_t>::const_iterator it=vecTgtPatterns.begin();
			it!=vecTgtPatterns.end(); it++)
	{
		if(!regexec(& (*it), in.c_str(), 0, NULL, 0))
			return true;
	}
	return false;
}

#endif //MINIBUILD


} // namespace rechecks

mstring GetDirPart(const string &in)
{
	if(in.empty())
		return "";

	tStrPos end = in.find_last_of(CPATHSEP);
	if(end == stmiss) // none? don't care then
		return "";

	return in.substr(0, end+1);
}

void mkbasedir(const string & path)
{
	if(0==mkdir(GetDirPart(path).c_str(), acfg::dirperms) || EEXIST == errno)
		return; // should succeed in most cases

	UINT pos=0; // but skip the cache dir components, if possible
	if(startsWith(path, acfg::cacheDirSlash))
	{
		// pos=acfg::cachedir.size();
		pos=path.find("/", acfg::cachedir.size()+1);
	}
    for(; pos<path.size(); pos=path.find(SZPATHSEP, pos+1))
    {
        if(pos>0)
            mkdir(path.substr(0,pos).c_str(), acfg::dirperms);
    }
}



/*
int main(int argc, char **argv)
{
	if(argc<2)
		return -1;
	
	acfg::tHostInfo hi;
	cout << "Parsing " << argv[1] << ", result: " << hi.SetUrl(argv[1])<<endl;
	cout << "Host: " << hi.sHost <<", Port: " << hi.sPort << ", Path: " << hi.sPath<<endl;
	return 0;
}
*/

