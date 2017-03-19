#ifndef _META_H
#define _META_H

#include "config.h"

#if defined(_POSIX_C_SOURCE) && (_POSIX_C_SOURCE < 200112L)
#undef _POSIX_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include <string>
#include <map>
#include <set>
#include <vector>
#include <deque>
#include <limits>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <functional>

#include <fcntl.h>
#include <pthread.h>
#include <strings.h>
#include <cstdlib>
#include <errno.h>

#define EXTREME_MEMORY_SAVING false


#ifdef _MSC_VER
#define __func__ __FUNCTION__
#endif

#if __GNUC__ == 4 && __GNUC_MINOR__ < 8 && !defined(__clang__)
#define COMPATGCC47
#define EMPLACE_PAIR_COMPAT(M,K,V) if((M).find(K) == (M).end()) (M).insert(std::make_pair(K,V))
#else
#define EMPLACE_PAIR_COMPAT(M,K,V) (M).emplace(K,V)
#endif

// little STFU helper
#if __GNUC__ >= 7
#define __just_fall_through [[fallthrough]]
#else
#define __just_fall_through
#endif

namespace acng
{

class acbuf;

typedef std::string mstring;
typedef const std::string cmstring;

typedef std::pair<mstring, mstring> tStrPair;
typedef std::vector<mstring> tStrVec;
typedef std::set<mstring> tStrSet;
typedef std::deque<mstring> tStrDeq;
typedef mstring::size_type tStrPos;
const static tStrPos stmiss(cmstring::npos);
typedef unsigned short USHORT;
typedef unsigned char UCHAR;
typedef const char * LPCSTR;
typedef std::pair<LPCSTR, size_t> tPtrLen;
#define citer const_iterator

#define CPATHSEPUNX '/'
#define SZPATHSEPUNIX "/"
#define CPATHSEPWIN '\\'
#define SZPATHSEPWIN "\\"
extern cmstring sPathSep, sPathSepUnix, sDefPortHTTP, sDefPortHTTPS, hendl;

extern cmstring FAKEDATEMARK;

#ifdef WINDOWS
#define WIN32
#define SZPATHSEP SZPATHSEPWIN
#define CPATHSEP CPATHSEPWIN
#define szNEWLINE "\r\n"
#else
#define SZPATHSEP SZPATHSEPUNIX
#define CPATHSEP CPATHSEPUNX
#define szNEWLINE "\n"
#endif

// some alternative versions of these flags

#ifndef O_NONBLOCK
#ifdef NOBLOCK
#define O_NONBLOCK NOBLOCK
#else
#ifdef O_NDELAY
#define O_NONBLOCK O_NDELAY
#endif
#endif
#endif

#ifndef O_NONBLOCK
#error "Unknown how to configure non-blocking mode (O_NONBLOCK) on this system"
#endif

//#define PATHSEP "/"
int getUUID();

#define SPACECHARS " \f\n\r\t\v"

#ifdef COMPATGCC47
class tStrMap : public std::map<mstring, mstring>
{
public:
	void emplace(cmstring& key, cmstring& value)
	{
		EMPLACE_PAIR_COMPAT(*this, key, value);
	}
};
#else
typedef std::map<mstring, mstring> tStrMap;
#endif

inline void trimFront(mstring &s, LPCSTR junk=SPACECHARS)
{
	mstring::size_type pos = s.find_first_not_of(junk);
	if(pos != 0)
		s.erase(0, pos);
}

inline void trimBack(mstring &s, LPCSTR junk=SPACECHARS)
{
	mstring::size_type pos = s.find_last_not_of(junk);
	s.erase(pos+1);
}

inline void trimString(mstring &s, LPCSTR junk=SPACECHARS)
{
	trimBack(s, junk);
	trimFront(s, junk);
}

#define trimLine(x) { trimFront(x); trimBack(x); }

#define startsWith(where, what) (0==(where).compare(0, (what).size(), (what)))
#define endsWith(where, what) ((where).size()>=(what).size() && \
		0==(where).compare((where).size()-(what).size(), (what).size(), (what)))
#define startsWithSz(where, what) (0==(where).compare(0, sizeof((what))-1, (what)))
#define endsWithSzAr(where, what) ((where).size()>=(sizeof((what))-1) && \
		0==(where).compare((where).size()-(sizeof((what))-1), (sizeof((what))-1), (what)))
#define stripSuffix(where, what) if(endsWithSzAr(where, what)) where.erase(where.size()-sizeof(what)+1);
#define stripPrefixChars(where, what) where.erase(0, where.find_first_not_of(what))

#define setIfNotEmpty(where, cand) { if(where.empty() && !cand.empty()) where = cand; }
#define setIfNotEmpty2(where, cand, alt) { if(where.empty()) { if(!cand.empty()) where = cand; else where = alt; } }

mstring GetBaseName(cmstring &in);
mstring GetDirPart(cmstring &in);
tStrPair SplitDirPath(cmstring& in);

LPCSTR GetTypeSuffix(cmstring& s);

void trimProto(mstring & sUri);
tStrPos findHostStart(const mstring & sUri);

#ifndef _countof
#define _countof(x) sizeof(x)/sizeof(x[0])
#endif

#define WITHLEN(x) x, (_countof(x)-1)
#define MAKE_PTR_0_LEN(x) x, 0, (_countof(x)-1)

// there is memchr and strpbrk but nothing like the last one acting on specified RAW memory range
static inline LPCSTR  mempbrk (LPCSTR  membuf, char const * const needles, size_t len)
{
   for(LPCSTR pWhere=membuf ; pWhere<membuf+len ; pWhere++)
      for(LPCSTR pWhat=needles; *pWhat ; pWhat++)
         if(*pWhat==*pWhere)
            return pWhere;
   return nullptr;
}

// Sometimes I miss Perl...
tStrVec::size_type Tokenize(cmstring &in, const char* sep, tStrVec & out, bool bAppend=false, mstring::size_type nStartOffset=0);
/*inline void Join(mstring &out, const mstring & sep, const tStrVec & tokens)
{out.clear(); if(tokens.empty()) return; for(const auto& tok: tokens)out+=(sep + tok);}
*/
void StrSubst(mstring &contents, const mstring &from, const mstring &to, tStrPos start=0);

// TODO: __attribute__((externally_visible))
bool ParseKeyValLine(const mstring & sIn, mstring & sOutKey, mstring & sOutVal);
#define keyEq(a, b) (0 == strcasecmp((a), (b).c_str()))

extern cmstring PROT_PFX_HTTPS, PROT_PFX_HTTP;

class tHttpUrl
{

private:
	mstring sPort;

public:
	bool SetHttpUrl(cmstring &uri, bool unescape = true);
	mstring ToURI(bool bEscaped) const;
	mstring sHost, sPath, sUserPass;

	bool bSSL=false;
	inline cmstring & GetProtoPrefix() const
	{
		return bSSL ? PROT_PFX_HTTPS : PROT_PFX_HTTP;
	}

	tHttpUrl & operator=(const tHttpUrl &a)
	{
		sHost = a.sHost;
		sPort = a.sPort;
		sPath = a.sPath;
		sUserPass = a.sUserPass;
		bSSL = a.bSSL;
		return *this;
	}
	bool operator==(const tHttpUrl &a) const
	{
		return a.sHost == sHost && a.sPort == sPort && a.sPath == sPath
				&& a.sUserPass == sUserPass && a.bSSL == bSSL;
	}
	;bool operator!=(const tHttpUrl &a) const
	{
		return !(a == *this);
	}
	inline void clear()
	{
		sHost.clear();
		sPort.clear();
		sPath.clear();
		sUserPass.clear();
		bSSL = false;
	}
	inline cmstring& GetDefaultPortForProto() const {
		return bSSL ? sDefPortHTTPS : sDefPortHTTP;
	}
	inline cmstring& GetPort() const { return !sPort.empty() ? sPort : GetDefaultPortForProto(); }

	inline tHttpUrl(cmstring &host, cmstring& port, bool ssl) :
			sPort(port), sHost(host), bSSL(ssl)
	{
	}
	inline tHttpUrl() =default;
	// evil method that should only be called for specific purposes in certain locations
	tHttpUrl* NormalizePath() { StrSubst(sPath, "//", "/"); return this; }
};

#define POKE(x) for(;;) { ssize_t n=write(x, "", 1); if(n>0 || (EAGAIN!=errno && EINTR!=errno)) break;  }

#define MIN_VAL(x) (std::numeric_limits< x >::min())
#define MAX_VAL(x) (std::numeric_limits< x >::max())

void appendLong(mstring &s, long val);

mstring BytesToHexString(const uint8_t b[], unsigned short binLength);
bool CsAsciiToBin(LPCSTR a, uint8_t b[], unsigned short binLength);

typedef const unsigned char CUCHAR;
bool CsEqual(LPCSTR a, uint8_t b[], unsigned short binLength);

#if SIZEOF_LONG == 8
// _FILE_OFFSET_BITS mostly irrelevant. But if it's set, watch out for user's "experiments".
#if _FILE_OFFSET_BITS == 32
#error Unsupported: _FILE_OFFSET_BITS == 32 with large long size
#else
#define OFF_T_FMT "%" PRId64
#endif

#else // not a 64bit arch?

#if 64 == _FILE_OFFSET_BITS
#define OFF_T_FMT "%" PRId64
#endif

#if 32 == _FILE_OFFSET_BITS
#define OFF_T_FMT "%" PRId32
#endif

#endif // !64bit arch

#ifndef OFF_T_FMT // either set above or let the os/compiler deal with the mess
#define OFF_T_FMT "%ld"
#endif

// let the compiler optimize and keep best variant
off_t atoofft(LPCSTR p);

inline off_t atoofft(LPCSTR p, off_t nDefVal)
{
	return p ? atoofft(p) : nDefVal;
}

mstring offttosH(off_t n);
mstring offttosHdotted(off_t n);
tStrDeq ExpandFilePattern(cmstring& pattern, bool bSorted=false, bool bQuiet=false);

//void MakeAbsolutePath(mstring &dirToFix, const mstring &reldir);


mstring UrlEscape(cmstring &s);
void UrlEscapeAppend(cmstring &s, mstring &sTarget);
bool UrlUnescapeAppend(cmstring &from, mstring & to);
// Decode with result as return value, no error reporting
mstring UrlUnescape(cmstring &from);
mstring DosEscape(cmstring &s);
// just the bare minimum to make sure the string does not break HTML formating
mstring html_sanitize(cmstring& in);

#define pathTidy(s) { if(startsWithSz(s, "." SZPATHSEP)) s.erase(0, 2); tStrPos n(0); \
	for(n=0;stmiss!=n;) { n=s.find(SZPATHSEP SZPATHSEP, n); if(stmiss!=n) s.erase(n, 1);}; \
	for(n=0;stmiss!=n;) { n=s.find(SZPATHSEP "." SZPATHSEP, n); if(stmiss!=n) s.erase(n, 2);}; }

// appears in the STL container?
#define ContHas(stlcont, needle) ((stlcont).find(needle) != (stlcont).end())
#define ContHasLinear(stlcont, needle) ((stlcont).end() != (std::find((stlcont).begin(), (stlcont).end(), needle)))

#define StrHas(haystack, needle) (haystack.find(needle) != stmiss)
#define StrHasFrom(haystack, needle, startpos) (haystack.find(needle, startpos) != stmiss)
#define StrEraseEnd(s,len) (s).erase((s).size() - len)

off_t GetFileSize(cmstring & path, off_t defret);

mstring offttos(off_t n);
mstring ltos(long n);
mstring offttosH(off_t n);

//template<typename charp>
off_t strsizeToOfft(const char *sizeString); // XXX: if needed... charp sizeString, charp *next)


void replaceChars(mstring &s, LPCSTR szBadChars, char goodChar);

extern cmstring sEmptyString;

//! iterator-like helper for string splitting, for convenient use with for-loops
class tSplitWalk
{
	cmstring &s;
	mutable mstring::size_type start, len, oob;
	LPCSTR m_seps;

public:
	inline tSplitWalk(cmstring *line, LPCSTR separators=SPACECHARS, unsigned begin=0)
	: s(*line), start(begin), len(stmiss), oob(line->size()), m_seps(separators) {}
	inline bool Next() const
	{
		if(len != stmiss) // not initial state, find the next position
			start = start + len + 1;

		if(start>=oob)
			return false;

		start = s.find_first_not_of(m_seps, start);

		if(start<oob)
		{
			len = s.find_first_of(m_seps, start);
			len = (len == stmiss) ? oob-start : len-start;
		}
		else if (len != stmiss) // not initial state, end reached
			return false;
		else if(s.empty()) // initial state, no parts
			return false;
		else // initial state, use the whole string
		{
			start = 0;
			len = oob;
		}

		return true;
	}
	inline mstring str() const { return s.substr(start, len); }
	inline operator mstring() const { return str(); }
	inline LPCSTR remainder() const { return s.c_str() + start; }
};

//bool CreateDetachedThread(void *(*threadfunc)(void *));

void DelTree(cmstring &what);

bool IsAbsolute(cmstring &dirToFix);

mstring unEscape(cmstring &s);

std::string BytesToHexString(const uint8_t sum[], unsigned short lengthBin);
//bool HexToString(const char *a, mstring& ret);
bool Hex2buf(const char *a, size_t len, acbuf& ret);

// STFU helpers, (void) casts are not effective for certain functions
static inline void ignore_value (int i) { (void) i; }
static inline void ignore_ptr (void* p) { (void) p; }

static inline time_t GetTime()
{
	return ::time(0);
}

static const time_t END_OF_TIME(MAX_VAL(time_t)-2);

unsigned FormatTime(char *buf, size_t bufLen, const time_t cur);

struct tCurrentTime
{
	char buf[30];
	unsigned len;
	inline tCurrentTime() { len=FormatTime(buf, sizeof(buf), time(nullptr)); }
	inline operator mstring() { return mstring(buf, len); }
};

// represents a boolean value like a normal bool but also carries additional data
template <typename Textra, Textra defval>
struct extended_bool
{
	bool value;
	Textra xdata;
	inline operator bool() { return value; }
	inline extended_bool(bool val, Textra xtra = defval) : value(val), xdata(xtra) {};
};

void DelTree(cmstring &what);

struct tErrnoFmter: public mstring
{
	tErrnoFmter(LPCSTR prefix = nullptr);
};

mstring EncodeBase64Auth(cmstring &sPwdString);
mstring EncodeBase64(LPCSTR data, unsigned len);

#if defined(HAVE_SSL) || defined(HAVE_TOMCRYPT)
#define HAVE_DECB64
bool DecodeBase64(LPCSTR pAscii, size_t len, acbuf& binData);
#endif

typedef std::deque<std::pair<std::string, std::string>> tLPS;

#ifdef __GNUC__
#define AC_LIKELY(x)   __builtin_expect(!!(x), true)
#define AC_UNLIKELY(x) __builtin_expect(!!(x), false)
#else
#define AC_LIKELY(x)   x
#define AC_UNLIKELY(x) x
#endif

// shortcut for the non-invasive lookup and copy of stuff from maps
#define ifThereStoreThere(x,y,z) { auto itFind = (x).find(y); if(itFind != (x).end()) z = itFind->second; }
#define ifThereStoreThereAndBreak(x,y,z) { auto itFind = (x).find(y); if(itFind != (x).end()) { z = itFind->second; break; } }

bool scaseequals(cmstring& a, cmstring& b);

// dirty little RAII helper
struct tDtorEx {
	std::function<void(void)> _action;
	inline tDtorEx(decltype(_action) action) : _action(action) {}
	inline ~tDtorEx() { _action(); }
};

// from bgtask.cc
cmstring GetFooter();

template<typename T>
std::pair<T,T> pairSum(const std::pair<T,T>& a, const std::pair<T,T>& b)
{
	return std::pair<T,T>(a.first+b.first, a.second + b.second);
}

}

#endif // _META_H

