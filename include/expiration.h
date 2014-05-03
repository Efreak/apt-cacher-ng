#ifndef EXPIRATION_H_
#define EXPIRATION_H_

#include "cacheman.h"

class expiration : public tCacheMan, ifileprocessor
{
public:

	enum workType
	{
		expire,
		list,
		purge,
		listDamaged,
		purgeDamaged,
		truncDamaged
	};
	expiration(int fd, workType type);
	virtual ~expiration();

protected:

	workType m_mode;

	tS2DAT m_trashCandSet;
	set<tFileNdir> m_trashCandHeadSet; // just collect the list of seen head files

	void RemoveAndStoreStatus(bool bPurgeNow);
	void LoadPreviousData(bool bForceInsert);

	// callback implementations
	virtual void Action(const mstring &);
	// for FileHandler
	virtual bool ProcessRegular(const mstring &sPath, const struct stat &);

	// for ifileprocessor
	virtual void HandlePkgEntry(const tRemoteFileInfo &entry, bool bUnpackForCsumming);

	virtual void UpdateFingerprint(const mstring &sPathRel, off_t nOverrideSize,
				uint8_t *pOverrideSha1, uint8_t *pOverrideMd5);

	void LoadHints();

	void PurgeMaintLogs();

	void DropExceptionalVersions();

	std::ofstream m_damageList;
	bool m_bIncompleteIsDamaged;

private:
	int m_nPrevFailCount;
	bool CheckAndReportError();
};

#endif /*EXPIRATION_H_*/
