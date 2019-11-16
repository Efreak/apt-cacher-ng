
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

namespace acng
{

struct tDlJob;
typedef std::shared_ptr<tDlJob> tDlJobPtr;
typedef std::list<tDlJobPtr> tDljQueue;

/**
 * dlcon is a basic connection broker for download processes.
 * It's defacto a slave of the conn class, the active thread is spawned by conn when needed
 * and it's finished by its destructor. However, the life time is prolonged if the usage count
 * is not down to zero, i.e. when there are more users registered as reader for the file
 * downloaded by the agent here then it will continue downloading and block the conn dtor
 * until that download is finished or the other client detaches. If a download is active and parent
 * conn object calls Stop... then the download will be aborted ASAP.
 *
 * Internally, a queue of download job items is maintained. Each contains a reference either to
 * a full target URL or to a tupple of a list of mirror descriptions (url prefix) and additional
 * path suffix for the required file.
 *
 * In addition, there is a local blacklist which is applied to all download jobs in the queue,
 * i.e. remotes marked as faulty there are no longer considered by the subsequent download jobs.
 */
class ACNG_API dlcon : public base_with_mutex
{ 
    public:
        dlcon(cmstring& sOwnersHostname, IDlConFactory *pConFactory = &g_tcp_con_factory);
        ~dlcon();

        void WorkLoop();
        
        void SignalStop();

        bool AddJob(tFileItemPtr m_pItem, const tHttpUrl *pForcedUrl,
        		const cfg::tRepoData *pRepoDesc,
        		cmstring *sPatSuffix, LPCSTR reqHead,
				int nMaxRedirection, const char* szHeaderXff);

    private:

    	//not to be copied
    	dlcon & operator=(const dlcon&);
    	dlcon(const dlcon&);
    	
    	friend struct tDlJob;
    	
    	tDljQueue m_qNewjobs;
    	IDlConFactory* m_pConFactory;
    	std::string m_ownersHostname;

#ifdef HAVE_LINUX_EVENTFD
    	int m_wakeventfd = -1;
#define fdWakeRead m_wakeventfd
#define fdWakeWrite m_wakeventfd
#else
    	int m_wakepipe[2] = {-1, -1};
#define fdWakeRead m_wakepipe[0]
#define fdWakeWrite m_wakepipe[1]
#endif
    	// flags and local copies for input parsing
    	/// remember being attached to an fitem

    	bool m_bStopASAP;

    	/// blacklist for permanently failing hosts, with error message
    	std::map<std::pair<cmstring,cmstring>, mstring> m_blacklist;
    	tSS m_sendBuf, m_inBuf;

    	unsigned ExchangeData(mstring &sErrorMsg, tDlStreamHandle &con, tDljQueue &qActive);

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
      void awaken_check();
};

#define IS_REDIRECT(st) (st == 301 || st == 302 || st == 307)

}

#endif
