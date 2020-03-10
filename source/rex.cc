/*
 * rex.cc
 *
 *  Created on: 09.03.2020
 *      Author: Eduard Bloch
 */

#include "acfg.h"
#include "rex.h"
#include "meta.h"
#include <regex.h>

using namespace std;

#define BADSTUFF_PATTERN "\\.\\.($|%|/)"

namespace acng
{
namespace rex
{

	// this has the exact order of the "regular" types in the enum
	struct { regex_t *pat=nullptr, *extra=nullptr; } rex[ematchtype_max];
	vector<regex_t> vecReqPatters, vecTgtPatterns;

void ACNG_API CompileExpressions()
{
	auto compat = [](regex_t* &re, const char* ps) ->void
	{
		if(!ps ||! *ps )
			return;
		re=new regex_t;
		int nErr=regcomp(re, ps, REG_EXTENDED);
		if(!nErr) return;

		char buf[1024];
		regerror(nErr, re, buf, sizeof(buf));
		delete re;
		re=nullptr;
		buf[_countof(buf)-1]=0; // better be safe...
		throw tStartupException(string("Regex compiler error at ") + ps);
		};
	using namespace cfg;
	compat(rex[FILE_SOLID].pat, pfilepat.c_str());
	compat(rex[FILE_VOLATILE].pat, vfilepat.c_str());
	compat(rex[FILE_WHITELIST].pat, wfilepat.c_str());
	compat(rex[FILE_SOLID].extra, pfilepatEx.c_str());
	compat(rex[FILE_VOLATILE].extra, vfilepatEx.c_str());
	compat(rex[FILE_WHITELIST].extra, wfilepatEx.c_str());
	compat(rex[NASTY_PATH].pat, BADSTUFF_PATTERN);
	compat(rex[FILE_SPECIAL_SOLID].pat, spfilepat.c_str());
	compat(rex[FILE_SPECIAL_SOLID].extra, spfilepatEx.c_str());
	compat(rex[FILE_SPECIAL_VOLATILE].pat, svfilepat.c_str());
	compat(rex[FILE_SPECIAL_VOLATILE].extra, svfilepatEx.c_str());

	if(connectPermPattern != "~~~") compat(rex[PASSTHROUGH].pat, connectPermPattern.c_str());
}

// match the specified type by internal pattern PLUS the user-added pattern
inline bool MatchType(const std::string &in, eMatchType type)
{
	if(rex[type].pat && !regexec(rex[type].pat, in.c_str(), 0, nullptr, 0))
		return true;
	if(rex[type].extra && !regexec(rex[type].extra, in.c_str(), 0, nullptr, 0))
		return true;
	return false;
}

bool Match(const std::string &in, eMatchType type)
{
	if(MatchType(in, type))
		return true;
	// very special behavior... for convenience
	return (type == FILE_SOLID && MatchType(in, FILE_SPECIAL_SOLID))
		|| (type == FILE_VOLATILE && MatchType(in, FILE_SPECIAL_VOLATILE));
}

ACNG_API eMatchType GetFiletype(const string & in)
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

inline bool CompileUncachedRex(const string & token, NOCACHE_PATTYPE type, bool bRecursiveCall)
{
	return true;
#warning FIXME. What was the spec? Add test!
#if 0
	auto & patvec = (NOCACHE_TGT == type) ? vecTgtPatterns : vecReqPatters;

	if (0!=token.compare(0, 5, "file:")) // pure pattern
	{
		unsigned pos = patvec.size();
		patvec.resize(pos+1);
		return 0==regcomp(&patvec[pos], token.c_str(), REG_EXTENDED);
	}
	else if(!bRecursiveCall) // don't go further than one level
	{
		tStrDeq srcs = cfg::ExpandFileTokens(token);
		for(const auto& src: srcs)
		{
			cfg::tCfgIter itor(src);
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
#endif
}


bool CompileUncExpressions(NOCACHE_PATTYPE type, cmstring& pat)
{
	return true;
#warning FIXME. What was the spec? Add test!
#if 0
	for(tSplitWalk split(pat); split.Next(); )
		if (!CompileUncachedRex(split, type, false))
			return false;
	return true;
#endif
}

bool MatchUncacheable(const string & in, NOCACHE_PATTYPE type)
{
	for(const auto& patre: (type == NOCACHE_REQ) ? vecReqPatters : vecTgtPatterns)
		if(!regexec(&patre, in.c_str(), 0, nullptr, 0))
			return true;
	return false;
}

LPCSTR ReTest(LPCSTR s)
{
	static LPCSTR names[rex::ematchtype_max] =
	{
				"FILE_SOLID", "FILE_VOLATILE",
				"FILE_WHITELIST",
				"NASTY_PATH", "PASSTHROUGH",
				"FILE_SPECIAL_SOLID"
	};
	auto t = rex::GetFiletype(s);
	if(t<0 || t>=rex::ematchtype_max)
		return "NOMATCH";
	return names[t];
}

} // namespace rechecks

} // acng
