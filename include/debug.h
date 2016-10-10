
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
#define LOGSTART(x)
#define LOGSTARTs(x)
#define LOGSTART2(x,y)
#define LOGSTART2s(x,y)
#define DBGQLOG(x)
#define dbgprint(x)
inline void dump_proc_status(){}; // strip away

#else

#define LOGLVL(n, x) if(acng::cfg::debug&n){ __logobj.GetFmter() << x; __logobj.Write(__FILE__,__LINE__); }
#define LOG(x) LOGLVL(LOG_DEBUG, x)

#define LOGSTARTFUNC t_logger __logobj(__func__, this);
#define LOGSTART(x) t_logger __logobj(x, this);
#define LOGSTARTs(x) t_logger __logobj(x, nullptr);
#define LOGSTART2(x, y) t_logger __logobj(x, this); LOGLVL(LOG_DEBUG, y /* << "@" __FILE__ ":" << __LINE__  */ )
#define LOGSTART2s(x, y) t_logger __logobj(x, nullptr); LOGLVL(LOG_DEBUG, y /*<< "@" __FILE__ ":" << __LINE__ */ )

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
