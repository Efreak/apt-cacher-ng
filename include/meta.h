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
#include <unordered_map>
#include <set>
#include <vector>
#include <deque>
#include <limits>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <functional>
#include <atomic>
#include <stdexcept>

#include <fcntl.h>
#include <pthread.h>
#include <strings.h>
#include <cstdlib>
#include <errno.h>

#include "astrop.h"

#define EXTREME_MEMORY_SAVING false


#ifdef _MSC_VER
#define __func__ __FUNCTION__
#endif
// take the best we can get in this case
#ifndef __GNUC__
#define __PRETTY_FUNCTION__ __func__
#endif


#ifdef __GNUC__
#define WARN_UNUSED  __attribute__ ((warn_unused_result))
#else
#define WARN_UNUSED
#endif

// little STFU helper
#if __GNUC__ >= 7
#define __just_fall_through [[fallthrough]]
#else
#define __just_fall_through
#endif

#define STRINGIFY(a) STR(a)
#define STR(a) #a

namespace acng
{

class acbuf;
struct tSysRes;

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
extern std::string sDefPortHTTP, sDefPortHTTPS;
extern cmstring sPathSep, sPathSepUnix, hendl;

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

static const cmstring sCRLF("\r\n");

extern uint_fast16_t hexmap[];
extern char h2t_map[];

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

typedef std::map<mstring, mstring> tStrMap;


LPCSTR GetTypeSuffix(cmstring& s);

void trimProto(mstring & sUri);
tStrPos findHostStart(const mstring & sUri);

#ifndef _countof
#define _countof(x) sizeof(x)/sizeof(x[0])
#endif

#define WITHLEN(x) x, (_countof(x)-1)
#define MAKE_PTR_0_LEN(x) x, 0, (_countof(x)-1)
#define MAKE_CHAR_PTR_SIZE(x, y) (const y *) (const char*)(& (x)), sizeof(x)

// there is memchr and strpbrk but nothing like the last one acting on specified RAW memory range
static inline LPCSTR  mempbrk (LPCSTR  membuf, char const * const needles, size_t len)
{
   for(LPCSTR pWhere=membuf ; pWhere<membuf+len ; pWhere++)
      for(LPCSTR pWhat=needles; *pWhat ; pWhat++)
         if(*pWhat==*pWhere)
            return pWhere;
   return nullptr;
}

#define ELVIS(x, y) (x ? x : y)
#define OPTSET(x, y) if(!x) x = y

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

struct ACNG_API tHostPortProto
{
protected:
	mstring sPort;
public:
	mstring sHost, sUserPass;
	bool bSSL=false;
	inline cmstring& GetDefaultPortForProto() const {
		return bSSL ? sDefPortHTTPS : sDefPortHTTP;
	}
	inline cmstring& GetPort() const { return !sPort.empty() ? sPort : GetDefaultPortForProto(); }
	bool operator==(const tHostPortProto& other) const
	{
		return other.sPort == sPort && other.sHost == sHost && other.bSSL == bSSL;
	}
    // arithmetics might be weird but it provides reliable order in efficient way
    bool operator<(const tHostPortProto& other) const
    {
        if( !(other.bSSL) != !(bSSL) )
            return !(other.bSSL) < !(bSSL);
        auto cm = other.sHost.compare(sHost);
        if(cm < 0)
            return true;
        if(cm > 0)
            return false;
        return sPort < other.sPort;

    }
	bool operator!=(const tHostPortProto& other) const
	{
		return !(other == *this);
	}
	tHostPortProto(cmstring &h, cmstring &port, bool bSsl) : sPort(port), sHost(h), bSSL(bSsl)
	{
	}
    tHostPortProto() =default;
    void SetPort(cmstring& newPort) { sPort = newPort; }
};

// XXX: better add a global custom specialization of std::hash
struct HostPortProtoHash
{
    unsigned operator()(const tHostPortProto& a) const
    {
        return (std::hash<std::string>()(a.sHost) ^ std::hash<std::string>()(a.GetPort())) + a.bSSL;
    }
};

class ACNG_API tHttpUrl : public tHostPortProto
{
	bool SetHttpUrlSafe(string_view uri);

public:
    using tHostPortProto::tHostPortProto; // ctor

	mstring sPath;
//	bool SetHttpUrl(cmstring &uri, bool unescape = true);
	bool SetHttpUrl(string_view uri, bool unescape = true);
	mstring ToURI(bool bEscaped) const;

	inline cmstring & GetProtoPrefix() const
	{
		return bSSL ? PROT_PFX_HTTPS : PROT_PFX_HTTP;
	}
	bool operator==(const tHttpUrl &a) const
	{
		return tHostPortProto::operator ==(a) && a.sPath == sPath;
	}
	bool operator!=(const tHttpUrl &a) const
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
	inline cmstring& GetPort(cmstring& szDefVal) const { return !sPort.empty() ? sPort : szDefVal; }
	inline cmstring& GetPort() const { return GetPort(GetDefaultPortForProto()); }

	// evil method that should only be called for specific purposes in certain locations
	tHttpUrl* NormalizePath() { StrSubst(sPath, "//", "/"); return this; }

	inline tStrPair HostPort() { return tStrPair(sHost, GetPort()); }

};

#define POKE(x) for(;;) { ssize_t n=write(x, "", 1); if(n>0 || (EAGAIN!=errno && EINTR!=errno)) break;  }

#define MIN_VAL(x) (std::numeric_limits< x >::min())
#define MAX_VAL(x) (std::numeric_limits< x >::max())

void appendLong(mstring &s, long val);

mstring BytesToHexString(const std::vector<uint8_t> theBytes);

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

ACNG_API mstring offttosH(off_t n);
ACNG_API mstring offttosHdotted(off_t n);
tStrDeq ExpandFilePattern(cmstring& pattern, bool bSorted=false, bool bQuiet=false);

//void MakeAbsolutePath(mstring &dirToFix, const mstring &reldir);


mstring UrlEscape(cmstring &s);
void UrlEscapeAppend(cmstring &s, mstring &sTarget);
//bool UrlUnescapeAppend(cmstring &from, mstring & to);
bool UrlUnescapeAppend(string_view from, mstring & to);
// Decode with result as return value, no error reporting
mstring UrlUnescape(string_view from);

mstring DosEscape(cmstring &s);
// just the bare minimum to make sure the string does not break HTML formating
mstring html_sanitize(cmstring& in);

ACNG_API mstring UserinfoEscape(cmstring &s);

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

ACNG_API mstring offttos(off_t n);
ACNG_API mstring ltos(long n);
ACNG_API mstring offttosH(off_t n);

//template<typename charp>
ACNG_API off_t strsizeToOfft(const char *sizeString); // XXX: if needed... charp sizeString, charp *next)

extern ACNG_API cmstring sEmptyString;

//bool CreateDetachedThread(void *(*threadfunc)(void *));

void DelTree(cmstring &what);
std::string PathCombine(string_view a, string_view b);
bool IsAbsolute(string_view dirToFix);

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
#if 0
// create a distinguishable timestamp with the highest resolution we can get, which is still ok for 32bit environment
static inline int64_t GetTimeStamp()
{
	timespec ts;
	if(0 == clock_gettime(CLOCK_MONOTONIC, &ts))
	{
		// 2 bits remaining, good enough until 23st century
		return (int64_t(ts.tv_sec) << 30) | int64_t(ts.tv_nsec);
	}
	timeval tv;
	if(0 == gettimeofday(&tv, nullptr))
	{
		return (int64_t(tv.tv_sec) << 30) | int64_t(tv.tv_sec*1000);
	}
	throw std::runtime_error("Bad clock or no permissions to fetch high precision time!");
}
#endif
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

void ACNG_API DelTree(cmstring &what);

struct ACNG_API tErrnoFmter: public mstring
{
	tErrnoFmter(LPCSTR prefix = nullptr);
	tErrnoFmter(LPCSTR prefix, int ec);
};

ACNG_API mstring EncodeBase64Auth(cmstring &sPwdString);
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

// mostly unique_ptr semantics for a non-pointer type with preserved invalid value, using a custom disposer function which is defined as static template parameter
template<typename T, void TDisposeFunction(T), T inval_default>
struct resource_owner
{
    T m_p;
    resource_owner() : m_p(inval_default) {}
    explicit resource_owner(T xp) : m_p(xp) {}
    ~resource_owner() { reset(); };
    T release() { auto ret=m_p; m_p = inval_default; return ret;}
    T get() const { return m_p; }
    T operator*() { return m_p; }
    resource_owner(const resource_owner&) = delete;
	resource_owner& reset(resource_owner &&other)
	{
		if (&other != this)
		{
			T orig = other.get();
			try
			{
				reset(other.release());
			}
			catch (...)
			{
				other.m_p = orig;
				throw;
			}
		}
		return *this;
	}
	resource_owner& reset(T xnew = inval_default)
	{
		if (xnew == m_p)
			return *this;
		if (m_p == inval_default)
		{
			m_p = xnew;
			return *this;
		}
		TDisposeFunction(m_p);
		m_p = xnew;
		return *this;
	}
	bool valid() const { return inval_default != m_p;}

    resource_owner(resource_owner && other)
	{
    	reset(other);
	}
    T operator=(resource_owner && other)
	{
    	return reset(other);
	}

};

// something similar to resource_owner but the owned object needs to be default-constructible and movable.
// This is made for non-primitive types which don't work with resource_owner due to non-type argument restrictions.
template<typename T, void TDisposeFunction(T)>
struct disposable_resource_owner
{
    T m_p;
    disposable_resource_owner() : m_p() {}
    explicit disposable_resource_owner(T xp) : m_p(xp) {}
    ~disposable_resource_owner() { TDisposeFunction(m_p); };
    T get() const { return m_p; }
    T operator*() { return m_p; }
    disposable_resource_owner(const disposable_resource_owner&) = delete;
    disposable_resource_owner(disposable_resource_owner && other)
	{
		m_p = move(other.m_p);
	}
    disposable_resource_owner& operator=(disposable_resource_owner &&other)
	{
		if (&other != this)
		{
			TDisposeFunction(m_p);
			m_p = move(other.m_p);
		}
		return *this;
	}
    void reset(T&& newVal)
    {
    	TDisposeFunction(m_p);
    	m_p = std::move(newVal);
    }
    /*
    disposable_resource_owner& operator=(T &&other)
	{
		if (&other != m_p)
		{
			TDisposeFunction(m_p);
			m_p = move(other.m_p);
		}
		return *this;
	}
	*/
};

// from bgtask.cc
cmstring GetFooter();

template<typename T>
std::pair<T,T> pairSum(const std::pair<T,T>& a, const std::pair<T,T>& b)
{
	return std::pair<T,T>(a.first+b.first, a.second + b.second);
}

#define RET_SWITCH(label) switch(label) {
#define RET_CASE(label) case label : goto label;
#define RET_SWITCH_END }

#define setLockGuardX(x) std::lock_guard<decltype(x)> local_helper_lockguard(x);

namespace cfg
{
extern int nettimeout;
}
struct CTimeVal
{
	struct timeval tv = {0,23};
public:
	// calculates for relative time (span)
	struct timeval* For(time_t tExpSec, suseconds_t tExpUsec = 23)
	{
		tv.tv_sec = tExpSec;
		tv.tv_usec = tExpUsec;
		return &tv;
	}
	struct timeval* ForNetTimeout()
	{
		tv.tv_sec = cfg::nettimeout;
		tv.tv_usec = 23;
		return &tv;
	}
	// calculates for absolute time
	struct timeval* Until(time_t tExpWhen, suseconds_t tExpUsec = 23)
	{
		tv.tv_sec = GetTime() + tExpWhen;
		tv.tv_usec = tExpUsec;
		return &tv;
	}
	// like above but with error checking
	struct timeval* SetUntil(time_t tExpWhen, suseconds_t tExpUsec = 23)
	{
		auto now(GetTime());
		if(now >= tExpWhen)
			return nullptr;
		tv.tv_sec = now + tExpWhen;
		tv.tv_usec = tExpUsec;
		return &tv;
	}
	// calculates for a timespan with max. length until tExpSec
	struct timeval* Remaining(time_t tExpSec, suseconds_t tExpUsec = 23)
	{
		auto exp = tExpSec - GetTime();
		tv.tv_sec = exp < 0 ? 0 : exp;
		tv.tv_usec = tExpUsec;
		return &tv;
	}
};
// special return type, has no other uses but compiler checks of controlled return flow
enum class srt
{
	a = 0
};

void justforceclose(int);
using unique_fd = resource_owner<int, justforceclose, -1>;

class tStartupException : public std::runtime_error
{
public:
	tStartupException(const std::string& s) : std::runtime_error(s)
	{
	}
};

}

#endif // _META_H

