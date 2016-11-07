
#ifndef _FILEITEM_H
#define _FILEITEM_H

#include <string>

#include "config.h"
#include "lockable.h"
#include "header.h"
#include <unordered_map>

namespace acng
{

class fileitem;
class fileitem_with_storage;
typedef std::shared_ptr<fileitem> tFileItemPtr;

//! Base class containing all required data and methods for communication with the download sources
class fileitem: public base_with_condition
{
	friend class fileitem_with_storage;

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
	
	virtual int GetFileFd();
	uint64_t GetTransferCount();
	// send helper like wrapper for sendfile. Just declare virtual here to make it better customizable later.
	virtual ssize_t SendData(int confd, int filefd, off_t &nSendPos, size_t nMax2SendNow)=0;
	
	// downloader instruments
	//typedef extended_bool<bool, false> SuccessWithTransErrorFlag;
	virtual bool DownloadStartedStoreHeader(const header & h, size_t hDataLen,
			const char *pNextData,
			bool bForcedRestart, bool &bDoCleanRestart) =0;
//	void IncDlRefCount();
//	void DecDlRefCount(const mstring & sReasonStatusLine);
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
	//bool m_bDisposed = false;
	fileitem();
	off_t m_nSizeChecked;
	header m_head;
	int m_filefd;
//	int m_nDlRefsCount;
//	int usercount;
	FiStatus m_status;
	mstring m_sPathRel;
	time_t m_nTimeDlStarted; //, m_nTimeDlDone;

};

// dl item implementation with storage on disk, can be shared among users
class fileitem_with_storage : public fileitem
{
public:
//	inline fileitem_with_storage(cmstring &s, int nUsers) {m_sPathRel=s; usercount=nUsers; };
	virtual ~fileitem_with_storage();
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

	/** Returns a (optionally new) globally registered item. If existingFi is supplied, that one will be registered.
	 * If one is supplied but another exists, a false pointer is returned.
	 */
	static tFileItemPtr CreateRegistered(cmstring& sPathRel,
			const tFileItemPtr& existingFi = tFileItemPtr());
	fileitem_with_storage(cmstring &s) {m_sPathRel=s;};

	// attempt to unregister a global item but only if it's unused
	static bool TryDispose(tFileItemPtr& existingFi);
protected:
	int MoveRelease2Sidestore();
};

#if 0 // murks
#ifndef MINIBUILD

// auto_ptr like object that "owns" a fileitem while it's registered in the global
// access table. Might cause item's deletion when destroyed
class fileItemMgmt
{
public:

	// public constructor wrapper, get a unique object from the map or a new one
	bool PrepareRegisteredFileItemWithStorage(cmstring &sPathUnescaped, bool bConsiderAltStore);

	// related to GetRegisteredFileItem but used for registration of custom file item
	// implementations created elsewhere (which still need to obey regular work flow)
	bool RegisterFileItem(tFileItemPtr spCustomFileItem);

	// deletes global registration and replaces m_ptr with another copy
	void RegisterFileitemLocalOnly(fileitem* replacement);

	//! @return: true iff there is still something in the pool for later cleaning
	static time_t BackgroundCleanup();

	static void dump_status();

	// when copied around, invalidates the original reference
	~fileItemMgmt();
	inline fileItemMgmt() {}
	inline tFileItemPtr getFiPtr() {return m_ptr;}
	inline operator bool() const {return (bool) m_ptr;}


private:
	tFileItemPtr m_ptr;
	void Unreg();

	fileItemMgmt(const fileItemMgmt &src);
	fileItemMgmt& operator=(const fileItemMgmt &src);

	inline fileitem* operator->() const {return m_ptr.get();}

};
#else
#define fileItemMgmt void
#endif // not MINIBUILD

#endif
}
#endif
