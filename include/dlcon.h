#ifndef _DLCON_H
#define _DLCON_H

namespace acng
{
class dlcon;

class IDlCon
{
	friend class ::acng::dlcon;
	virtual ~IDlCon();

public:

	virtual bool AddJob(tFileItemPtr m_pItem, const tHttpUrl *pForcedUrl,
			const cfg::tRepoData *pBackends,
			cmstring *sPatSuffix, LPCSTR reqHead,
			int nMaxRedirection) = 0;

	/**
	 * Master conn agent is terminating. Downloader can act individually as
	 * long as needed and can terminate then.
	 *
	 * @return true if downloader needs to be destroyed now, false if downloader
	 * has outstanding work running and will commit suicide later.
	 */
	bool BecomeRonin() =0;

	/**
	 * Adds an action to run when state changes
	 * @return A key that caller has to use to remove its action handler again.
	 */
	const void* subscribe(tAction updateAction) =0;
	void unsubscribe(const void *key) =0;

	static IDlCon* MakeInstance(cmstring& xff = sEmptyString, int forcedConnectionFd = -1);
};
}

#endif
