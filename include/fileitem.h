
#ifndef _FILEITEM_H
#define _FILEITEM_H

#include <string>

#include "config.h"
#include "lockable.h"
#include "header.h"
#include <unordered_map>
#include <atomic>

namespace acng
{

class fileitem;
class fileitem_with_storage;
class job;
typedef std::shared_ptr<fileitem> tFileItemPtr;

//! Base class containing all required data and methods for communication with the download sources
class fileitem
{
	friend class fileitem_with_storage;
	public:

	// Life cycle (process states) of a file description item
	enum FiStatus : char
	{

	FIST_FRESH, // constructed
	FIST_INITIALIZING, // initial checks in filesystem are active
	FIST_INITED, // initial checks done, might be ready to start delivery
	FIST_DLASSIGNED, // downloader was assigned but there might still some retry scheme running
	FIST_DLGOTHEAD, // valid header was received
	FIST_DLRECEIVING, // data transfer is active
	FIST_COMPLETE, // the item is downloaded, no further transfer needed

	// error cases: downloader reports its error or last user told downloader to stop
	FIST_DLERROR
	};

	virtual ~fileitem();
	
	virtual int GetFileFd();
//	uint64_t GetTransferCount();

	// Abstraction for the appropriate way to send data to the acng client
	// Can be just a wrapper for sendfile.
	virtual ssize_t SendData(int confd, int filefd, off_t &nSendPos, size_t nMax2SendNow)=0;
	
	// dlcon callbacks:
	virtual bool DownloadStartedStoreHeader(const header & h, size_t hDataLen,
			const char *pNextData,
			bool bForcedRestart, bool &bDoCleanRestart) =0;
	/*!
	 * \return true IFF ok and caller might continue. False -> caller should abort.
	 */
	virtual bool StoreFileData(const char *data, unsigned int size)=0;
	inline header GetHeaderLocking() { lockguard g(m_mx);  return m_head; }
	
	void subscribe(int fd);
	void unsubscribe(int fd);

	FiStatus GetStatus(pthread_t *dlThreadId = nullptr, mstring* psHttpStatusOrErrorMsg = nullptr,
			off_t *nConfirmedSizeSoFar = nullptr, header *retHead=nullptr);
	/*!
	 * Extracts the HTTP status line from the header, in the format "status-code message".
	 * If status is error, the message will be 500 or higher.
	 */
	inline cmstring GetHttpStatus()
	{
		mstring ret;
		GetStatus(0, &ret);
		if(ret.empty()) ret = "500 Unknown Error";
		return ret;

	}
	/*
			(m_stateInternal == STATE_SEND_MAIN_HEAD)
			? &respHead : nullptr)
	{ lockguard g(m_mx);
	return m_status;
	}
	*/
//	FiStatus GetStatusUnlocked(off_t &nGoodDataSize) { nGoodDataSize = m_nUsableSizeInCache; return m_status; }
//	void ResetCacheState();

	//! returns true if complete or DL not started yet but partial file is present and contains requested range and file contents is static
//	bool CheckUsableRange_unlocked(off_t nRangeLastByte);

	// returns when the state changes to complete or error
	FiStatus WaitForFinish(int *httpCode=nullptr);

	bool ResetCacheState(bool bForce=false);
	
	/// mark the item as complete as-is, assuming that sizeseen is correct
	void SetComplete();

	void UpdateHeadTimestamp();

	// shall only be called when the object is in safe state, i.e. from download thread
	inline const header& GetHeaderUnlocked() { return m_head; }

	// Initialise file item (exactly once), return the status
	virtual FiStatus SetupFromCache(bool bDynType);

	bool m_bHeadOnly = false; // only for pass-through mode, write/read by conn thread only

	bool m_bCheckFreshness = true; // if-modified-since or if-range flags required; set only once before FIST_INITED

	bool m_bIsGloballyRegistered = false; // defacto a flag identifying it as fileitem_with_storage (globally-registered)

#warning fixme
//protected:

	fileitem();

//private:
	tInOutCounters m_intOutCounters; // written and read by the conn/dlcon thread only
	off_t m_nSizeSeenInCache = 0;   // the best known information about total size of the file. Initially set by conn thread, updated by ANY other dlcon thread during FIST_DLRECEIVING phase.
	off_t m_nCheckedSize = 0; // the available validated data range for the current download; policy as for m_nSizeSeenInCache
	off_t m_nRangeLimit = -1;	// only for pass-though mode, write/read by conn thread only

	// local cached values for dlcon callbacks only
	bool m_bAllowStoreData = true;
	int m_filefd = -1;

	// poke everyone who subscribed for updates
	void notifyObservers();

	// policy: only increasing value; read any time, write before FIST_INITED by creating thread, after by first assigned downloader thread
	FiStatus m_status {FIST_FRESH};

	// access protected by mutex
	header m_head;
  
	mstring m_sPathRel; // policy: assign in ctor, change never

	// protects access to values updated by dl-thread and read by others
	acmutex m_mx;

	// remember which thread was assigned as downloader
	pthread_t m_dlThreadId = {0};

	std::deque<int> m_subscribers;

	// just a shortcut
	void poke(int fd);
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
	 *
	 * If create is needed, bDynType will be passed to the SetupFromStorage call
	 */
	static tFileItemPtr CreateRegistered(cmstring& sPathRel,
			const tFileItemPtr& existingFi = tFileItemPtr(), bool *created4caller = nullptr);
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
