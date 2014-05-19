#ifndef EXPIRATION_H_
#define EXPIRATION_H_

#include "cacheman.h"

class expiration : public tCacheOperation, ifileprocessor
{
public:
	using tCacheOperation::tCacheOperation;

protected:

	tS2DAT m_trashCandSet;
	set<tFileNdir> m_trashCandHeadSet; // just collect the list of seen head files

	void RemoveAndStoreStatus(bool bPurgeNow);
	void LoadPreviousData(bool bForceInsert);

	// callback implementations
	virtual void Action(const mstring &) override;
	// for FileHandler
	virtual bool ProcessRegular(const mstring &sPath, const struct stat &) override;

	// for ifileprocessor
	virtual void HandlePkgEntry(const tRemoteFileInfo &entry, bool bUnpackForCsumming) override;

	virtual void UpdateFingerprint(const mstring &sPathRel, off_t nOverrideSize,
				uint8_t *pOverrideSha1, uint8_t *pOverrideMd5) override;

	void LoadHints();

	void PurgeMaintLogs();

	void DropExceptionalVersions();

	MYSTD::ofstream m_damageList;
	bool m_bIncompleteIsDamaged = false;

private:
	int m_nPrevFailCount =0;
	bool CheckAndReportError();
};

#endif /*EXPIRATION_H_*/
