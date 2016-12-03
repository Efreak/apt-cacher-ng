
#ifndef _FILEITEM_H
#define _FILEITEM_H

#include <fileitem.h>
#include <string>

#include "config.h"
#include "lockable.h"
#include "header.h"
#include <unordered_map>
#include <atomic>

namespace acng
{

struct fileitem;
class tFileItemEx;
class job;
typedef std::shared_ptr<fileitem> tFileItemPtr;

//! Base class containing all required data and methods for communication with the download sources
struct fileitem
{
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

	FiStatus GetStatus(mstring* psHttpStatusOrErrorMsg = nullptr,
			off_t *nConfirmedSizeSoFar = nullptr, header *retHead=nullptr);
	/*!
	 * Extracts the HTTP status line from the header, in the format "status-code message".
	 * If status is error, the message will be 500 or higher.
	 */
	inline cmstring GetHttpStatus()
	{
		mstring ret;
#warning crap, fix
		//GetStatus(&ret);
		if(ret.empty()) ret = "500 Unknown Error";
		return ret;

	}

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

	bool m_bIsGloballyRegistered = false; // defacto a flag identifying it as tFileItemEx (globally-registered)

	fileitem();

#if TRACK_OUTCOUNT
	tInOutCounters m_inOutCounters;
#else
	off_t m_nIncommingCount = 0; // written and read by the conn/dlcon thread only
#endif
	off_t m_nSizeSeenInCache = 0;   // the best known information about total size of the file. Initially set by conn thread, updated by ANY other dlcon thread during FIST_DLRECEIVING phase.
	off_t m_nCheckedSize = 0; // the available validated data range for the current download; policy as for m_nSizeSeenInCache
	off_t m_nRangeLimit = -1;	// only for pass-though mode, write/read by conn thread only

	// local cached values for dlcon callbacks only
	bool m_bAllowStoreData = true;
	int m_filefd = -1;

	// set a new status and notify thread waiting on m_cvState
	void SetReportStatus(FiStatus);

	// policy: only increasing value; read any time, write before FIST_INITED by creating thread, after by first assigned downloader thread
	FiStatus m_status {FIST_FRESH};

	// access protected by mutex
	header m_head;

	mstring m_sPathRel; // policy: assign in ctor, change never

	// protects access to values updated by dl-thread and read by others
	std::mutex m_mx;

	// remember which thread was assigned as downloader
	pthread_t m_dlThreadId = {0};

	std::vector<int> m_subscribers;

	// only used for blocking clients
	std::condition_variable m_cvState;

	// ... with lock
	void notifyObservers();

protected:

	// poke everyone who subscribed for updates
	void notifyObserversNoLock();

	// just a shortcut
	void poke(int fd);
};

/**
 * Extended implementation of file item object, supporting:
 * a) global registration under specific name
 * b) code to process incoming data
 *
 * XXX: it's still slightly spaghetti style since tPassThroughFitem makes use of it
 * directly, although disabling cache-modification code first
 */
class tFileItemEx : public fileitem
{
public:
//	inline tFileItemEx(cmstring &s, int nUsers) {m_sPathRel=s; usercount=nUsers; };
	virtual ~tFileItemEx();
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
	tFileItemEx(cmstring &s) {m_sPathRel=s;};

	// attempt to unregister a global item but only if it's unused
	static bool TryDispose(tFileItemPtr& existingFi);
protected:
	int MoveRelease2Sidestore();
};

}
#endif
