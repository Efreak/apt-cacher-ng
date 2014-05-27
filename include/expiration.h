#ifndef EXPIRATION_H_
#define EXPIRATION_H_

#include "cacheman.h"
#include <list>
#include <unordered_map>

// caching all relevant file identity data and helper flags in such entries
struct tDiskFileInfo
{
	time_t nLostAt =0;
	off_t filesize=0;
	bool bNoHeaderCheck=false;
};


class expiration : public tCacheOperation, ifileprocessor
{
public:
	using tCacheOperation::tCacheOperation;

protected:

	MYSTD::unordered_map<mstring,MYSTD::map<mstring,tDiskFileInfo>> m_trashFile2dir2Info;

	//tS2DAT m_trashCandSet;
	//set<tFileNdir> m_trashCandHeadSet; // just collect the list of seen head files

	void RemoveAndStoreStatus(bool bPurgeNow);
	void LoadPreviousData(bool bForceInsert);

	// callback implementations
	virtual void Action(const mstring &) override;
	// for FileHandler
	virtual bool ProcessRegular(const mstring &sPath, const struct stat &) override;

	// for ifileprocessor
	virtual void HandlePkgEntry(const tRemoteFileInfo &entry) override;

	void LoadHints();

	void PurgeMaintLogs();

	void DropExceptionalVersions();

	MYSTD::ofstream m_damageList;
	bool m_bIncompleteIsDamaged = false;

#warning vielleicht doch was cooleres überlegen, off_t als indexe in den vector missbrauchen oder pointer in eine liste hinein oder alternativ-implementierung überhaupt
	MYSTD::unordered_map<uintptr_t,tFingerprint> m_fprCache;

private:
	int m_nPrevFailCount =0;
	bool CheckAndReportError();
};

#endif /*EXPIRATION_H_*/
