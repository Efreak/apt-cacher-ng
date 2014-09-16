
#ifndef _DLCON_H
#define _DLCON_H

#include <string>
#include <list>
#include <map>
#include <set>

//#include <netinet/in.h>
//#include <netdb.h>

#include "tcpconnect.h"
#include "lockable.h"
#include "fileitem.h"
#include "acfg.h"
#include "acbuf.h"

struct tDlJob;
typedef SHARED_PTR<tDlJob> tDlJobPtr;
typedef std::list<tDlJobPtr> tDljQueue;

class dlcon : public lockable
{ 
    public:
        dlcon(bool bManualExecution, mstring *xff=NULL);
        ~dlcon();

        void WorkLoop();
        
        void SignalStop();

        bool AddJob(tFileItemPtr m_pItem, tHttpUrl *pForcedUrl=nullptr,
        		const acfg::tRepoData *pRepoDesc=nullptr,
        		cmstring *sPatSuffix=nullptr);

        mstring m_sXForwardedFor;

    private:

    	//not to be copied
    	dlcon & operator=(const dlcon&);
    	dlcon(const dlcon&);
    	
    	friend struct tDlJob;
    	
    	tDljQueue m_qNewjobs;

#ifdef HAVE_LINUX_EVENTFD
    	int m_wakeventfd = -1;
#define fdWakeRead m_wakeventfd
#define fdWakeWrite m_wakeventfd
#else
    	int m_wakepipe[] = {-1, -1};
#define fdWakeRead m_wakepipe[0]
#define fdWakeWrite m_wakepipe[1]
#endif
    	// flags and local copies for input parsing
    	/// remember being attached to an fitem

    	bool m_bStopASAP;

    	UINT m_bManualMode;

    	/// blacklist for permanently failing hosts, with error message
    	std::map<std::pair<cmstring,cmstring>, mstring> m_blacklist;
    	tSS m_sendBuf, m_inBuf;

    	UINT ExchangeData(mstring &sErrorMsg, tTcpHandlePtr &con, tDljQueue &qActive);

    	// Disable pipelining for the next # requests. Actually used as crude workaround for the
    	// concept limitation (because of automata over a couple of function) and its
    	// impact on download performance.
    	// The use case: stupid web servers that redirect all requests do that step-by-step, i.e.
    	// they get a bunch of requests but return only the first response and then flush the buffer
    	// so we process this response and wish to switch to the new target location (dropping
    	// the current connection because we don't keep it somehow to background, this is the only
    	// download agent we have). This manner perverts the whole principle and causes permanent
    	// disconnects/reconnects. In this case, it's beneficial to disable pipelining and send
    	// our requests one-by-one. This is done for a while (i.e. the valueof(m_nDisablePling)/2 )
    	// times before the operation mode returns to normal.
    	int m_nTempPipelineDisable;


    	// the default behavior or using or not using the proxy. Will be set
    	// if access proxies shall no longer be used.
    	bool m_bProxyTot;

    	// this is a binary factor, meaning how many reads from buffer are OK when
    	// speed limiting is enabled
    	unsigned m_nSpeedLimiterRoundUp = (unsigned(1)<<16)-1;
    	unsigned m_nSpeedLimitMaxPerTake = MAX_VAL(unsigned);
      unsigned m_nLastDlCount=0;

      void wake();
};

#define IS_REDIRECT(st) (st == 301 || st == 302 || st == 307)

#endif


