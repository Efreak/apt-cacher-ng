#ifndef _DLCON_H
#define _DLCON_H

#include "tcpconfactory.h"

namespace acng
{
class fileitem;

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
class ACNG_API dlcon
{
	class Impl;
	Impl *_p;

    public:
        dlcon(cmstring& sOwnersHostname, IDlConFactory &pConFactory = GetTcpConFactory());
        ~dlcon();

        void WorkLoop();
        void SignalStop();
        bool AddJob(SHARED_PTR<fileitem> m_pItem, const tHttpUrl *pForcedUrl,
        		const cfg::tRepoData *pRepoDesc,
        		cmstring *sPatSuffix, LPCSTR reqHead,
				int nMaxRedirection, bool isPassThroughRequest);
};

#define IS_REDIRECT(st) (st == 301 || st == 302 || st == 307)

}

#endif
