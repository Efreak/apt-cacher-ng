
#ifndef _FILEITEM_H
#define _FILEITEM_H

#include <string>
#include <memory>
#include <unordered_map>

#include "config.h"
#include "lockable.h"
#include "header.h"
#include "meta.h"

namespace acng
{

// XXX: make this configurable - VolatileItemCacheTime
#define MAXTEMPDELAY acng::cfg::maxtempdelay // 27
class fileitem;
typedef SHARED_PTR<fileitem> tFileItemPtr;
void StopUsingFileitem(tFileItemPtr);

using TFileItemUser = disposable_resource_owner<tFileItemPtr,StopUsingFileitem>;

//! Base class containing all required data and methods for communication with the download sources
//#warning the whole locking scheme sucks. Can and should redo with inhererent safety, and use the lock requirements appropriate for the purpose and not more. https://en.cppreference.com/w/cpp/thread/shared_timed_mutex
// XXX: OTOH adding a reader-lock to every getter would suck too, many wasted cycles
class ACNG_API fileitem : public base_with_condition
{

public:

	mstring m_sCachePathRel;

	enum class ESharingStrategy
	{
		ALWAYS_ATTACH, // jump on the bandwagon in any case, even with old items
		REPLACE_AS_NEEDED, // on conflict, auto-decide what to do with the old item (just close or move aside and drop later)
//		REPLACE_IF_RETIRED, // retire/replace an item which is old and download has finished/stopped and remained only in the quick cool-down cache
//		REPLACE_IF_TOO_SLOW,	// retire/replace even a still active item
		ALWAYS_REPLACE, // install a fresh item and push the old aside, in any case
//		ALWAYS_REPLACE_AND_RESET // make it a new item and make sure that old cached contents are destroyed ASAP
	};

	// Life cycle (process states) of a file description item
	enum FiStatus : char
	{
		// temporary state
	FIST_FRESH, FIST_INITED, FIST_DLPENDING, FIST_DLGOTHEAD, FIST_DLRECEIVING,
	// final states:
	FIST_COMPLETE,
	// error cases: downloader reports its error or last user told downloader to stop
	//! Stopped with an internal error
	FIST_DLERROR,
	//! Download activities stopped, no further changes
	FIST_DLSTOP
	};
	
	fileitem(cmstring& s) : m_sCachePathRel(s) {}
	virtual ~fileitem() {};

	// Initialise file item, return the status
	virtual FiStatus Setup(bool bDynType);
	
	virtual unique_fd GetFileFd() =0;
	uint64_t GetTransferCount();
	// send helper like wrapper for sendfile. Just declare virtual here to make it better customizable later.
	virtual ssize_t SendData(int confd, int filefd, off_t &nSendPos, size_t nMax2SendNow)=0;
	
	// downloader instruments
	//typedef extended_bool<bool, false> SuccessWithTransErrorFlag;
	virtual bool DownloadStarted(const header & h, size_t hDataLen,
			const char *pNextData,
			bool bForcedRestart, bool &bDoCleanRestart);

	/**
	 * Shortcut for insertion of file and header data from a donor file.
	 * Prerequisite: Setup() was not done yet
	 * Returns: bit field
	 * 1 insertion succeeded
	 * 2 source was destroyed
	 * 4 aborted because another download is active
	 */
	virtual short Set(cmstring srcPathRel, cmstring srcHeadRel, const header* h, bool sacrificeSource) {return false;}
	/**
	 * For special implementations, member the original header data.
	 * By default, drop that data since we pickup everything important already.
	 */
	virtual void SetRawResponseHeader(std::string) {}
	virtual const std::string& GetRawResponseHeader() { return sEmptyString; }


	//! Detect when the last possible downloader disappears for any reason and mark this as error state then
	void IncDlRefCount();
	void DecDlRefCount(const mstring & sReasonStatusLine);

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

	// BS! Replace with retirement scheme, if needed whatsoever
//	bool SetupClean(bool bForce=false);
	
	/// mark the item as complete as-is, assuming that sizeseen is correct
	void SetupComplete();

	uint64_t m_nIncommingCount = 0;
	off_t m_nSizeSeen = -1;
	off_t m_nRangeLimit = -1;
	
	bool m_bCheckFreshness = true;
	// those is only good for very special purposes [tm]
	bool m_bHeadOnly = false;

protected:
	bool m_bAllowStoreData = true;
	//! See SinkDeprecate()
	bool m_bIsDeprecated = false;
	//! Kill the cached data when finished
#warning implement
	bool m_AutoDestroy = false;
	//! Hint for user/release scheme to avoid unnecessary checks
	bool m_bWasShared = false;
#warning setzen wenn es in dingsda kommt

	off_t m_nSizeChecked = -1;
	header m_head;
	int m_nDlRefsCount = 0;
	std::atomic_int usercount = ATOMIC_VAR_INIT(0);
	FiStatus m_status = FIST_FRESH;
#warning bei allen zuweisungen timestamp updaten!
	//! Marks a timestamp of significant change (especially relevant for FIST_RETIRE timer and ESharingStrategy checks)
	time_t m_nTimestampStatusChange = 0;

	friend void ::acng::StopUsingFileitem(tFileItemPtr);

public:
	// public constructor wrapper, create a sharable item with storage or share an existing one
	static TFileItemUser Create(cmstring &sPathUnescaped, ESharingStrategy collisionHint) WARN_UNUSED;

	// related to GetRegisteredFileItem but used for registration of custom file item
	// implementations created elsewhere (which still need to obey regular work flow)
	static TFileItemUser Install(tFileItemPtr spCustomFileItem, ESharingStrategy collisionHint)  WARN_UNUSED;

	/**
	 * Server just a single local file.
	 */
	static TFileItemUser Create(cmstring& absPath, const struct stat& statInfo)  WARN_UNUSED;

	//! @return: true iff there is still something in the pool for later cleaning
	static time_t BackgroundCleanup();

	static void dump_status();


	/*
	 *
	 * Data sink interface
	 *
	 */

	/**
	 * Update the internal mark for the most recently used file analysis.
	 */
	virtual void UpdateUseDate() =0;
	/*!
	 * \return true IFF ok and caller might continue. False -> caller should abort.
	 */
	virtual bool StoreBody(const char *data, unsigned int size) =0;
	/**
	 * Destroy cached data.
	 */
	virtual void SinkDestroy(bool delHed, bool delData) =0;

	/**
	 * Moves the item and its contents to a side lane and out of the way.
	 * It's still usable but the contents will be lost in the end.
	 *
	 * PRECONDITION: Item is not shared
	 */
#warning crazy idea, remove ASAP
	virtual bool SinkMoveOutOfTheWay() { return false; };

protected:

	//virtual bool StoreHeader(const header& h) =0;
	/**
	 * Instructions on how to deal with the cache entity.
	 * Instructions can be executed in a row (see code), with or without error checking.
	 * Normal priority: trim -> sync -> close
	 * @param errorAbort If true, stop on the first error (errno can be checked)
	 * @param sync Sync. file data via OS
	 * @param trim Adust file size to m_nSizeChecked - use with care!
	 * @param close Close the file handle ASAP
	 */
	virtual bool SinkCtrl(bool errorAbort, bool sync, bool trim, bool close) =0;
	virtual const char* SinkOpen(off_t hint_start, off_t hint_length) =0;
	virtual void DetectNotModifiedDownload(char upcomingByte, off_t reportedSize) =0;

	static TFileItemUser Install(std::function<tFileItemPtr()> lazyItemConstructor, cmstring& sPathRel, ESharingStrategy collisionHint);
};

typedef std::unordered_map<std::string,SHARED_PTR<fileitem> /*, hashfib, eq_fib*/> tFiGlobMap;

}
#endif
