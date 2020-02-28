
//#define LOCAL_DEBUG
#include "debug.h"

#include "config.h"
#include "fileitem.h"
#include "header.h"
#include "acfg.h"
#include "acbuf.h"
#include "cleaner.h"

#include <errno.h>
#include <algorithm>
#include <unordered_map>
using namespace std;

namespace acng
{

#if 0
struct hashfib
{
	std::size_t operator()(cmstring* ps) const noexcept
	{
		return ps ? std::hash<std::string>(*ps) : 0;
	}
};
struct eq_fib
{
	bool operator()(cmstring* ps, cmstring* pt) const noexcept
	{
		// XXX: needed? if(!ps) ps = &sEmptyString; if(!pt) pt = &sEmptyString;
		return *ps == *pt;
	}
};
#endif
// lookup for the shared file items. The key is a plain pointer, in order to save some memory; this okay because the cache path remains the same as long as the item is shared
tFiGlobMap mapItems;
acmutex mapItemsMx;
std::deque<TFileItemUser> orderedExpQueue;

header const & fileitem::GetHeaderUnlocked()
{
	return m_head;
}

string fileitem::GetHttpMsg()
{
	setLockGuard;
	if(m_head.frontLine.length()>9)
		return m_head.frontLine.substr(9);
	return m_head.frontLine;
}

void fileitem::IncDlRefCount()
{
	setLockGuard;
	m_nDlRefsCount++;
}

void fileitem::DecDlRefCount(const string &sReason)
{
	setLockGuard;

	notifyAll();

	m_nDlRefsCount--;
	if(m_nDlRefsCount>0)
		return; // someone will care...

	// ... otherwise: the last downloader disappeared, needing to tell observers

	if (m_status<FIST_COMPLETE)
	{
		m_status=FIST_DLERROR;
		m_head.clear();
		m_head.frontLine=string("HTTP/1.1 ")+sReason;
		m_head.type=header::ANSWER;

		if (cfg::debug&log::LOG_MORE)
			log::misc(string("Download of ")+m_sCachePathRel+" aborted");
	}
	SinkCtrl(false, false, true, true);
}

uint64_t fileitem::GetTransferCount()
{
	setLockGuard;
	uint64_t ret=m_nIncommingCount;
	m_nIncommingCount=0;
	return ret;
}

void fileitem::ResetCacheState()
{
	setLockGuard;
	m_nSizeSeen = 0;
	m_nSizeChecked = 0;
	m_status = FIST_FRESH;
	m_bAllowStoreData = true;
	m_head.clear();
}

fileitem::FiStatus fileitem::Setup(bool bCheckFreshness)
{
	LOGSTART2("fileitem::Setup", bCheckFreshness);

	setLockGuard;

	if(m_status>FIST_FRESH)
		return m_status;

	m_status=FIST_INITED;
	m_bCheckFreshness = bCheckFreshness;

	cmstring sPathAbs(CACHE_BASE+m_sCachePathRel);

	if(m_head.LoadFromFile(sPathAbs+".head") >0 && m_head.type==header::ANSWER )
	{
		if(200 != m_head.getStatus())
			goto error_clean;


		LOG("good head");

		m_nSizeSeen=GetFileSize(sPathAbs, 0);

		// some plausibility checks
		if(m_bCheckFreshness)
		{
			const char *p=m_head.h[header::LAST_MODIFIED];
			if(!p)
				goto error_clean; // suspicious, cannot use it
			LOG("check freshness, last modified: " << p );

			// that will cause check by if-mo-only later, needs to be sure about the size here
			if(cfg::vrangeops == 0
					&& m_nSizeSeen != atoofft(m_head.h[header::CONTENT_LENGTH], -17))
			{
				m_nSizeSeen = 0;
			}
		}
		else
		{
			// non-volatile files, so could accept the length, do some checks first
			const char *pContLen=m_head.h[header::CONTENT_LENGTH];
			if(pContLen)
			{
				off_t nContLen=atoofft(pContLen); // if it's 0 then we assume it's 0

				// file larger than it could ever be?
				if(nContLen < m_nSizeSeen)
					goto error_clean;

				LOG("Content-Length has a sane range");

				m_nSizeChecked=m_nSizeSeen;

				// is it complete? and 0 value also looks weird, try to verify later
				if(m_nSizeSeen == nContLen && nContLen>0)
					m_status=FIST_COMPLETE;
			}
			else
			{
				// no content length known, assume it's ok
				m_nSizeChecked=m_nSizeSeen;
			}
		}
	}
	else // -> no .head file
	{
		// maybe there is some left-over without head file?
		// Don't thrust volatile data, but otherwise try to reuse?
		if(!bCheckFreshness)
			m_nSizeSeen=GetFileSize(sPathAbs, 0);
	}
	LOG("resulting status: " << (int) m_status);
	return m_status;

	error_clean:
	SinkDestroy(true, false);
	m_head.clear();
	m_nSizeSeen=0;
	m_status=FIST_INITED;
	return m_status; // unuseable, to be redownloaded
}

bool fileitem::CheckUsableRange_unlocked(off_t nRangeLastByte)
{
	if(m_status == FIST_COMPLETE)
		return true;
	if(m_status < FIST_INITED || m_status > FIST_COMPLETE)
		return false;
	if(m_status >= FIST_DLGOTHEAD)
		return nRangeLastByte > m_nSizeChecked;

	// special exceptions for static files
	return (m_status == FIST_INITED && !m_bCheckFreshness
			&& m_nSizeSeen>0 && nRangeLastByte >=0 && nRangeLastByte <m_nSizeSeen
			&& atoofft(m_head.h[header::CONTENT_LENGTH], -255) > nRangeLastByte);
}

#if 0
#warning FIXME: bForce is brute force and may have unwanted side effects
bool fileitem::SetupClean(bool bForce)
{
	setLockGuard;

	if(bForce)
	{
		if(m_status>FIST_FRESH)
		{
			m_status = FIST_DLERROR;
			m_head.frontLine="HTTP/1.1 500 FIXME, DEAD ITEM";
		}
	}
	else
	{
		if(m_status>FIST_FRESH)
			return false;
		m_status=FIST_INITED;
	}
	cmstring sPathAbs(SABSPATH(m_sCachePathRel));
	cmstring sPathHead(sPathAbs+".head");
	// header allowed to be lost in process...
	//	if(unlink(sPathHead.c_str()))
	//		::ignore_value(::truncate(sPathHead.c_str(), 0));
	acng::ignore_value(::truncate(sPathAbs.c_str(), 0));
	Cstat stf(sPathAbs);
	if(stf && stf.st_size>0)
		return false; // didn't work. Permissions? Anyhow, too dangerous to act on this now
	header h;
	h.LoadFromFile(sPathHead);
	h.del(header::CONTENT_LENGTH);
	h.del(header::CONTENT_TYPE);
	h.del(header::LAST_MODIFIED);
	h.del(header::XFORWARDEDFOR);
	h.del(header::CONTENT_RANGE);
	h.StoreToFile(sPathHead);
	//	if(0==stat(sPathHead.c_str(), &stf) && stf.st_size >0)
	//		return false; // that's weird too, header still exists with real size
	m_head.clear();
	m_nSizeSeen=m_nSizeChecked=0;

	return true;
}
#endif

void fileitem::SetupComplete()
{
	setLockGuard;
	notifyAll();
	m_nSizeChecked = m_nSizeSeen;
	m_status = FIST_COMPLETE;
}


fileitem::FiStatus fileitem::WaitForFinish(int *httpCode)
{
	lockuniq g(this);
	while(m_status<FIST_COMPLETE)
		wait(g);
	if(httpCode)
		*httpCode=m_head.getStatus();
	return m_status;
}

bool fileitem::DownloadStarted(const header & h, size_t hDataLen,
		const char *pNextData,
		bool bForcedRestart, bool &bDoCleanRetry)
{
	LOGSTART("fileitem::DownloadStartedStoreHeader");
	auto SETERROR = [&](LPCSTR x) {
		m_bAllowStoreData=false;
		m_head.frontLine=mstring("HTTP/1.1 ")+x;
		m_head.set(header::XORIG, h.h[header::XORIG]);
		m_status=FIST_DLERROR; m_nTimestampStatusChange=GetTime();
		_LogWithErrno(x, m_sCachePathRel);
	};

	auto withError = [&](LPCSTR x) {
		SETERROR(x);
		return false;
	};

	setLockGuard;

	USRDBG( "Download started, storeHeader for " << m_sCachePathRel << ", current status: " << (int) m_status);
	
	if(m_status >= FIST_COMPLETE)
	{
		USRDBG( "Download was completed or aborted, not restarting before expiration");
		return false;
	}

	// conflict with another thread's download attempt? Deny, except for a forced restart
	if (m_status > FIST_DLPENDING && !bForcedRestart)
		return false;

	m_nTimestampStatusChange = GetTime();

	m_nIncommingCount+=hDataLen;

	// optional optimization: hints for the filesystem resp. kernel
	off_t hint_start(0), hint_length(0);

	// status will change, most likely... ie. return withError action
	notifyAll();

//	cmstring sPathAbs(CACHE_BASE+m_sPathRel);
//	string sHeadPath=sPathAbs + ".head";

	auto withErrorAndKillFile = [&](LPCSTR x)
			{
		SETERROR(x);
		LOG("Deleting " << m_sCachePathRel);
		this->SinkDestroy(true, true);
		m_status=FIST_DLERROR;
		m_nTimestampStatusChange=GetTime();
		return false;
			};

	int serverStatus = h.getStatus();
	switch(serverStatus)
	{
	case 200:
	{
		if(m_status < FIST_DLGOTHEAD)
			bForcedRestart = false; // behave normally, set all data

		if (bForcedRestart)
		{
			if (m_nSizeChecked != 0)
			{
				/* shouldn't be here, server should have resumed at the previous position.
				 * Most likely the remote file was modified after the download started.
				 */
				//USRDBG( "state: " << m_status << ", m_nsc: " << m_nSizeChecked);
				return withError("500 Failed to resume remote download");
			}
			if(h.h[header::CONTENT_LENGTH] && atoofft(h.h[header::CONTENT_LENGTH])
					!= atoofft(h.h[header::CONTENT_LENGTH], -2))
			{
				return withError("500 Failed to resume remote download, bad length");
			}
			m_head.set(header::XORIG, h.h[header::XORIG]);
		}
		else
		{
			m_nSizeChecked=0;
			m_head=h;
		}
		hint_length=atoofft(h.h[header::CONTENT_LENGTH], 0);
		break;
	}
	case 206:
	{

		if(m_nSizeSeen<=0 && m_nRangeLimit<0)
		{
			// wtf? Cannot have requested partial content
			return withError("500 Unexpected Partial Response");
		}
		/*
		 * Range: bytes=453291-
		 * ...
		 * Content-Length: 7271829
		 * Content-Range: bytes 453291-7725119/7725120
		 */
		const char *p=h.h[header::CONTENT_RANGE];
		if(!p)
			return withError("500 Missing Content-Range in Partial Response");
		off_t myfrom, myto, mylen;
		int n=sscanf(p, "bytes " OFF_T_FMT "-" OFF_T_FMT "/" OFF_T_FMT, &myfrom, &myto, &mylen);
		if(n<=0)
			n=sscanf(p, "bytes=" OFF_T_FMT "-" OFF_T_FMT "/" OFF_T_FMT, &myfrom, &myto, &mylen);

		ldbg("resuming? n: "<< n << " und myfrom: " <<myfrom << 
				" und myto: " << myto << " und mylen: " << mylen);
		if(n!=3  // check for nonsense
				|| (m_nSizeSeen>0 && myfrom != m_nSizeSeen-1)
				|| (m_nRangeLimit>=0 && myto != m_nRangeLimit)
				|| myfrom<0 || mylen<0
		)
		{
			return withError("500 Server reports unexpected range");
		}

		m_nSizeChecked=myfrom;

		hint_start=myfrom;
		hint_length=mylen;

		m_head=h;
		m_head.frontLine="HTTP/1.1 200 OK";
		m_head.del(header::CONTENT_RANGE);
		m_head.set(header::CONTENT_LENGTH, mylen);
		m_head.set(header::XORIG, h.h[header::XORIG]);

		// target opened before? close it, will reopen&seek later
		if (bForcedRestart)
			SinkCtrl(false, false, true, true);

		// special optimization; if "-1 trick" was used then maybe don't reopen that file for writing later
		if(m_bCheckFreshness && pNextData)
			DetectNotModifiedDownload(*pNextData, mylen);
		break;
	}
	case 416:
		// that's bad; it cannot have been completed before (the -1 trick)
		// however, proxy servers with v3r4 cl3v3r caching strategy can cause that
		// if if-mo-since is used and they don't like it, so attempt a retry in this case
		if(m_nSizeChecked == 0)
		{
			USRDBG( "Peer denied to resume previous download (transient error) " << m_sCachePathRel );
			m_nSizeSeen = 0;
			bDoCleanRetry=true;
			return false;
		}
		else
		{
			// -> kill cached file ASAP
			m_bAllowStoreData=false;
			m_head.copy(h, header::XORIG);
			return withErrorAndKillFile("503 Server disagrees on file size, cleaning up");
		}
		break;
	default:
		m_head.type=header::ANSWER;
		m_head.copy(h, header::XORIG);
		m_head.copy(h, header::LOCATION);
		if(bForcedRestart)
		{
			// got an error from the replacement mirror? cannot handle it properly
			// because some job might already have started returning the data
			USRDBG( "Cannot restart, HTTP code: " << serverStatus);
			return withError(h.getCodeMessage());
		}

		m_bAllowStoreData = false;
		// have a clean header with just the error message
		m_head.frontLine = h.frontLine;
		m_head.set(header::CONTENT_LENGTH, "0");
		if(m_status > FIST_DLGOTHEAD)
		{
			// oh shit. Client may have already started sending it. Prevent such trouble in future.
			SinkDestroy(true, true);
#warning double-check all usescases
		}
	}

	if(cfg::debug & log::LOG_MORE)
		log::misc(string("Download of ")+m_sCachePathRel+" started");

	if(m_bAllowStoreData)
	{
		auto ret = SinkOpen(hint_start, hint_length);
		if(ret)
			return withError(ret);
	}

	m_status=FIST_DLGOTHEAD;
	return true;
}

void StopUsingFileitem(tFileItemPtr ptr)
{
	if (!ptr) // unregistered before? or not shared?
		return;

	lockguard managementLock(mapItemsMx);

	auto ucount = --ptr->usercount;
	// still used or was never shared? Then ignore
	if (ucount > 0 || !ptr->m_bWasShared)
		return;

#warning FIXME, completely broken

#if 0
	auto it = mapItems.find(&ptr->m_sCachePathRel);
	// not there or is not us
	if(it == mapItems.end() || !it->second || ptr.get() != it->second.get())
		return;
#warning not during shutdown!
	// ok, can we retire this? Like keep it usable for a short time?
	if(ptr->m_status == fileitem::FIST_COMPLETE && ptr->m_bCheckFreshness && MAXTEMPDELAY > 0)
	{
		ptr->m_status = fileitem::FIST_RETIRED;
		ptr->m_nTimestampStatusChange = GetTime();
		tRetiredReference hintExp {ptr.get(), it};
		orderedExpQueue.emplace_back(move(hintExp));
//FIXME: stopgap value, can do better?
		cleaner::GetInstance().ScheduleFor(ptr->m_nTimestampStatusChange + MAXTEMPDELAY, cleaner::TYPE_EXFILEITEM);
	}
	else
	{
		mapItems.erase(it);
	}
#endif
}

// make the fileitem globally accessible
TFileItemUser fileitem::Install(tFileItemPtr spCustomFileItem, fileitem::ESharingStrategy onConflict)
{
	if (!spCustomFileItem || spCustomFileItem->m_sCachePathRel.empty())
		return TFileItemUser();
	return Install([&spCustomFileItem]() { return spCustomFileItem; }, spCustomFileItem->m_sCachePathRel, onConflict);
	/*

	if(!isShareable)
		return TFileItemUser(spCustomFileItem);


	auto installed = mapItems.emplace(spCustomFileItem->m_sPathRel,
			spCustomFileItem);

	if(!installed.second)
		return TFileItemUser(); // conflict, another agent is already active

	spCustomFileItem->m_globRef = installed.first;
	spCustomFileItem->usercount++;
	return TFileItemUser(spCustomFileItem);
	*/
}

// this method is supposed to be awaken periodically and detects items with ref count manipulated by
// the request storm prevention mechanism. Items shall be be dropped after some time if no other
// thread but us is using them.
time_t fileitem::BackgroundCleanup()
{
	LOGSTART2s("fileItemMgmt::BackgroundCleanup", GetTime());
	lockguard lockGlobalMap(mapItemsMx);

	time_t now = GetTime();
	time_t oldestGet = END_OF_TIME;
	time_t expBefore = now - MAXTEMPDELAY;
#warning restore
#if 0
	for(auto it = mapItems.begin(); it != mapItems.end();)
	{
		auto here = it++;

		// became busy again? Ignore this entry, it's new master will take care of the deletion ASAP
		if(here->second->usercount >0)
			continue;

		// find and ignore (but remember) the candidate(s) for the next cycle
		if (here->second->m_nTimeDlStarted > expBefore)
		{
			oldestGet = std::min(time_t(here->second->m_nTimeDlStarted), oldestGet);
			continue;
		}

		// ok, unused and delay is over. Destroy with the same sequence as mgmt destructor does,
		// care about life time.
		tFileItemPtr local_ptr(here->second);
		{
			lockguard g(*local_ptr);
			local_ptr->m_status = fileitem::FIST_DLSTOP;
			local_ptr->m_globRef = mapItems.end();
			mapItems.erase(here);
		}
		local_ptr->notifyAll();
	}

	if(oldestGet == END_OF_TIME)
		return oldestGet;

	ldbg(oldestGet);
#endif
	// preserving a few seconds to catch more of them in the subsequent run
	return std::max(oldestGet + MAXTEMPDELAY, GetTime() + 8);
}

void fileitem::dump_status()
{

#warning restore me, status dumper
#if 0
	tSS fmt;
	log::err("File descriptor table:\n");
	for(const auto& it : mapItems)
	{
		if(!it.first) continue;
		auto& item = * static_cast<fileitem*>(it.second.get());
		fmt.clear();
		fmt << "FREF: " << &item << " [" << item.usercount << "]:\n";

			fmt << "\t" << item.m_sCachePathRel
					<< "\n\tDlRefCount: " << item.m_nDlRefsCount
					<< "\n\tState: " << (int)  item.m_status
					<< "\n\tFilePos: " << item.m_nIncommingCount << " , "
					<< item.m_nRangeLimit << " , "
					<< item.m_nSizeChecked << " , "
					<< item.m_nSizeSeen
					<< "\n\tLast-State-Change: " << item.m_nTimestampStatusChange << "\n\n";

		log::err(fmt);
	}
	log::flush();

#endif
}

TFileItemUser fileitem::Install(std::function<tFileItemPtr()> lazyItemConstructor, cmstring& sKeyPathRel, ESharingStrategy collisionHint)
{
#warning FIXME: restore/redo all this logics
	return TFileItemUser();
#if 0
	auto item2use = [](tFileItemPtr& p) {if(p) p->usercount++; return TFileItemUser(p);};
	lockguard lockGlobalMap(mapItemsMx);
	auto it = mapItems.find(sKeyPathRel);
	if(it != mapItems.end())
	{
		if(collisionHint == ESharingStrategy::ALWAYS_ATTACH)
			return item2use(it->second);
		bool justClose = collisionHint == ESharingStrategy::
		switch(collisionHint)
		{
		case ESharingStrategy::ALWAYS_ATTACH: ;
		case ESharingStrategy::ALWAYS_REPLACE:
		{
			tFileItemPtr x;
			x.swap(it->second);
			x->SinkMoveOutOfTheWay();
			it->second = lazyItemConstructor();
			return item2use(it->second);
		}
		}
		// ok, we might need a conflict resolution, how would it look like?
		if(collisionHint == ESharingStrategy::ALWAYS_ATTACH)
			{
			return TFileItemUser(it->second);
			}


#error BS, hier die zustände behandeln
//		|| (ESharingStrategy::REPLACE_IF_RETIRED == collisionHint && it->second->m_status == FIST_RETIRED))
		auto& fi = it->second;
		bool doRetire = collisionHint == ESharingStrategy::REPLACE_IF_TEMPCACHED);
#warning eval werte ab da
		if(!fi->SinkRetire())
			return TFileItemUser();

		LOG("Sharing existing file item");
		it->second->usercount++;
		return TFileItemUser(it->second);
	}
	else
	{
#error bs, where is registering parts
		return TFileItemUser(itemFactory());
	}



	sp->usercount++;
#error not here, shall do in Install()
	auto res = mapItems.emplace(sPathRel, sp);
	ASSERT(res.second);
	sp->m_globRef = res.first;
	return sp;
#endif

}

}
