
#ifndef __DEBUG_H__
#define __DEBUG_H__

#include "acfg.h"
#include "aclogger.h"
#include "meta.h"

#include <fstream>
#include <iostream>
#ifdef DEBUG
#include <assert.h>
#endif

namespace acng
{

#ifdef DEBUG
#define ASSERT(x) assert(x)
#else
#define ASSERT(x)
#endif

#ifndef DEBUG
#define ldbgvl(v, x)
#define dbglvl(v)
#define ldbg(x)
#define dbgline
#define ASSERT(x)
#define LOG(x)
#define LOGSTARTFUNC

#define LOGSTARTFUNC
#define LOGSTARTFUNCs
#define LOGSTARTFUNCx(...)
#define LOGSTARTFUNCsx(...)
#define LOGSTARTFUNCxs(...)
#define LOGSTART(x)
#define LOGSTARTx(x, ...)
#define LOGSTARTs(x)
#define LOGSTARTsx(x, ...)
#define DBGQLOG(x)
#define dbgprint(x)
#define LOGRET(x) return x;
inline void dump_proc_status(){}; // strip away

#else

/*
 * They are all too heavy
#ifdef __GNUC__
#define __ACFUNC__  __PRETTY_FUNCTION__
//#elif MSCVER
//#define __ACFUNC__ __FUNCSIG__
#else
#define __ACFUNC__ __func__
#endif
*/
#define __ACFUNC__ __func__


#define LOGVA(n, pfx, ...) if(acng::cfg::debug & n) \
		{ auto&f=__logobj.GetFmter(pfx); tSS::Chain(f, ", ", __VA_ARGS__); \
			__logobj.WriteWithContext(__FILE__ ":" STRINGIFY(__LINE__)); }

#define LOGAPP(n, pfx, x) if(acng::cfg::debug&n){ __logobj.GetFmter(pfx) << x; __logobj.WriteWithContext(__FILE__ ":" STRINGIFY(__LINE__)); }

#define LOG(what) LOGAPP(log::LOG_DEBUG, "- ", what)

#define LOGSTART(x) t_logger __logobj(x, this);
#define LOGSTARTs(x) t_logger __logobj(x, nullptr);
#define LOGSTARTx(nam, ...) LOGSTART(nam); LOGVA(log::LOG_DEBUG, "PARMS: ", __VA_ARGS__);
#define LOGSTARTsx(nam, ...) LOGSTARTs(nam); LOGVA(log::LOG_DEBUG, "PARMS: ", __VA_ARGS__);
//#define LOGSTART(x, y) t_logger __logobj(x, this); LOGLVL(log::LOG_DEBUG, y )
//#define LOGSTART2s(x, y) t_logger __logobj(x, nullptr); LOGLVL(log::LOG_DEBUG, y)
#define LOGRET(x) { __logobj.GetFmter4End() << " --> " << x << " @" __FILE__ ":" STRINGIFY(__LINE__); return x; }

#define LOGSTARTFUNC LOGSTART(__ACFUNC__)
#define LOGSTARTFUNCs LOGSTARTs(__ACFUNC__)
#define LOGSTARTFUNCx(...) LOGSTARTx(__ACFUNC__, __VA_ARGS__)
#define LOGSTARTFUNCsx(...) LOGSTARTsx(__ACFUNC__, __VA_ARGS__)
#define LOGSTARTFUNCxs(...) LOGSTARTFUNCsx(__VA_ARGS__)


#define dbgprint(x) std::cerr << x << std::endl;

#define ldbg(x) LOG(x)

#define dbgline ldbg("mark")
#define DBGQLOG(x) {log::err(tSS()<< x);}
#define dump_proc_status dump_proc_status_always

#endif


inline void dump_proc_status_always()
{
	using namespace std;
	ifstream sf("/proc/self/status");
	while (sf)
	{
		string s;
		getline(sf, s);
		cerr << s << endl;
	}
};

}

#endif // __DEBUG_H__
