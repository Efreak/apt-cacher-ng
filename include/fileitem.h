
#ifndef _FILEITEM_H
#define _FILEITEM_H

#include <string>

#include "config.h"
#include "lockable.h"
#include "header.h"
#include "fileio.h"
#include <unordered_map>

namespace acng
{

class fileitem;
typedef std::shared_ptr<fileitem> tFileItemPtr;
typedef std::unordered_map<mstring, tFileItemPtr> tFiGlobMap;
void StopUsingFileitem(tFileItemPtr);
using TFileItemUser = resource_owner_default<tFileItemPtr,StopUsingFileitem>;

//! Base class containing all required data and methods for communication with the download sources
class ACNG_API fileitem : public base_with_condition
{
public:

	// Life cycle (process states) of a file description item
	enum FiStatus : char
	{

	FIST_FRESH, FIST_INITED, FIST_DLPENDING, FIST_DLGOTHEAD, FIST_DLRECEIVING,
	FIST_COMPLETE,
	// error cases: downloader reports its error or last user told downloader to stop
	FIST_DLERROR,
	FIST_DLSTOP // assumed to not have any users left
	};

	virtual ~fileitem();
	
	// initialize file item, return the status
	virtual FiStatus Setup(bool bDynType);
	
	virtual unique_fd GetFileFd();
	uint64_t GetTransferCount();
	// send helper like wrapper for sendfile. Just declare virtual here to make it better customizable later.
	virtual ssize_t SendData(int confd, int filefd, off_t &nSendPos, size_t nMax2SendNow)=0;
	
	// downloader instruments
	//typedef extended_bool<bool, false> SuccessWithTransErrorFlag;
	virtual bool DownloadStartedStoreHeader(const header & h, size_t hDataLen,
			const char *pNextData,
			bool bForcedRestart, bool &bDoCleanRestart) =0;
	void IncDlRefCount();
	void DecDlRefCount(const mstring & sReasonStatusLine);
	//virtual void SetFailureMode(const mstring & message, FiStatus fist=FIST_ERROR,
	//	bool bOnlyIfNoDlRunnuning=false);
	
	/*!
	 * \return true IFF ok and caller might continue. False -> caller should abort.
	 */
	virtual bool StoreFileData(const char *data, unsigned int size)=0;
	header const & GetHeaderUnlocked();
	inline header GetHeader() { setLockGuard; return m_head; }
	mstring GetHttpMsg();
	
	FiStatus GetStatus() { setLockGuard; return m_status; }
	FiStatus GetStatusUnlocked(off_t &nGoodDataSize) { nGoodDataSize = m_nSizeChecked; return m_status; }
	void ResetCacheState();

	//! returns true if complete or DL not started yet but partial file is present and contains requested range and file contents is static
	bool CheckUsableRange_unlocked(off_t nRangeLastByte);

	// returns when the state changes to complete or error
	FiStatus WaitForFinish(int *httpCode=nullptr);

	bool SetupClean(bool bForce=false);
	
	/// mark the item as complete as-is, assuming that sizeseen is correct
	void SetupComplete();

	void UpdateHeadTimestamp();

	uint64_t m_nIncommingCount;
	off_t m_nSizeSeen;
	off_t m_nRangeLimit;
	
	bool m_bCheckFreshness;
	// those is only good for very special purposes [tm]
	bool m_bHeadOnly;

protected:
	bool m_bAllowStoreData;
	fileitem();
	off_t m_nSizeChecked;
	header m_head;
	int m_filefd;
	int m_nDlRefsCount;
	std::atomic_int usercount = ATOMIC_VAR_INIT(0);
	FiStatus m_status;
	mstring m_sPathRel;
	time_t m_nTimeDlStarted, m_nTimeDlDone;
	virtual int Truncate2checkedSize() {return 0;};

protected:
	// this is owned by fileitem and covered by its locking; it serves as flag for shared objects and a self-reference for fast and exact deletion
	tFiGlobMap::iterator m_globRef;
	friend void ::acng::StopUsingFileitem(tFileItemPtr);

public:
	// public constructor wrapper, create a sharable item with storage or share an existin one
	static TFileItemUser Create(cmstring &sPathUnescaped, bool bConsiderAltStore) WARN_UNUSED;

	// related to GetRegisteredFileItem but used for registration of custom file item
	// implementations created elsewhere (which still need to obey regular work flow)
	static TFileItemUser Create(tFileItemPtr spCustomFileItem, bool isShareable)  WARN_UNUSED;

	//! @return: true iff there is still something in the pool for later cleaning
	static time_t BackgroundCleanup();

	static void dump_status();
};

// dl item implementation with storage on disk
class fileitem_with_storage : public fileitem
{
public:
	inline fileitem_with_storage(cmstring &s) {m_sPathRel=s;};
	virtual ~fileitem_with_storage();
	int Truncate2checkedSize() override;
	// send helper like wrapper for sendfile. Just declare virtual here to make it better customizable later.
	virtual ssize_t SendData(int confd, int filefd, off_t &nSendPos, size_t nMax2SendNow) override;
	virtual bool DownloadStartedStoreHeader(const header & h, size_t hDataLen,
			const char *pNextData,
			bool bForcedRestart, bool&) override;
	virtual bool StoreFileData(const char *data, unsigned int size) override;

	inline static mstring NormalizePath(cmstring &sPathRaw)
	{
		return cfg::stupidfs ? DosEscape(sPathRaw) : sPathRaw;
	}
protected:
	int MoveRelease2Sidestore();
};

}
#endif
