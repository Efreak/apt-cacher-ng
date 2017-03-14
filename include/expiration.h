#ifndef EXPIRATION_H_
#define EXPIRATION_H_

#include "cacheman.h"
#include <list>
#include <unordered_map>

namespace acng
{

// caching all relevant file identity data and helper flags in such entries
struct tDiskFileInfo
{
	time_t nLostAt =0;
	bool bNoHeaderCheck=false;
	// this adds a couple of procent overhead so it's negligible considering
	// hashing or traversing overhead of a detached solution
	tFingerprint fpr;
};

class expiration : public cacheman
{
public:
	// XXX: g++ 4.7 is not there yet... using tCacheOperation::tCacheOperation;
	inline expiration(const tRunParms& parms) : cacheman(parms) {};

protected:

	std::unordered_map<mstring,std::map<mstring,tDiskFileInfo>> m_trashFile2dir2Info;
	tStrVec m_oversizedFiles;

	void RemoveAndStoreStatus(bool bPurgeNow);
	void LoadPreviousData(bool bForceInsert);

	// callback implementations
	virtual void Action() override;
	// for FileHandler
	virtual bool ProcessRegular(const mstring &sPath, const struct stat &) override;
	void HandlePkgEntry(const tRemoteFileInfo &entry);

	void LoadHints();

	void PurgeMaintLogs();

	void DropExceptionalVersions();

	std::ofstream m_damageList;
	bool m_bIncompleteIsDamaged = false, m_bScanVolatileContents = false;

	void MarkObsolete(cmstring&) override;
	tStrVec m_killBill;

	virtual bool _checkSolidHashOnDisk(cmstring& hexname, const tRemoteFileInfo &entry,
			cmstring& srcPrefix) override;
	virtual bool _QuickCheckSolidFileOnDisk(cmstring& /* sFilePathRel */) override;
private:
	int m_nPrevFailCount =0;
	bool CheckAndReportError();

	void HandleDamagedFiles();
	void ListExpiredFiles();
	void TrimFiles();
};

}

#endif /*EXPIRATION_H_*/
