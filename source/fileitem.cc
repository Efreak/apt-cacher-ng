
//#define LOCAL_DEBUG
#include "debug.h"

#include "config.h"
#include "fileitem.h"
#include "header.h"
#include "acfg.h"
#include "acbuf.h"
#include "fileio.h"
#include "cleaner.h"
#include "filelocks.h"
#ifdef HAVE_LINUX_EVENTFD
#include <sys/eventfd.h>
#endif

#include <errno.h>
#include <algorithm>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>

using namespace std;

namespace acng
{
#define MAXTEMPDELAY acng::cfg::maxtempdelay // 27
mstring sReplDir("_altStore" SZPATHSEP);

typedef std::unordered_multimap<mstring, std::weak_ptr<fileitem> > tFiGlobMap;

static tFiGlobMap g_sharedItems;
static acmutex g_sharedItemsMx;
/*
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
*/

fileitem::fileitem()
{
}

fileitem::~fileitem()
{
	// assuming that it's in sane state and closing it here is only precaution
	checkforceclose(m_filefd);
}

#if 0
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
			log::misc(string("Download of ")+m_sPathRel+" aborted");
	}
	checkforceclose(m_filefd);
}
#endif

/*
uint64_t fileitem::GetTransferCount()
{
	setLockGuard;
	uint64_t ret=m_nIncommingCount;
	m_nIncommingCount=0;
	return ret;
}
*/

int fileitem::GetFileFd() {
	LOGSTART("fileitem::GetFileFd");

	// sufficient checks
	if(m_status < FIST_DLRECEIVING || m_status > FIST_COMPLETE)
		return -1;
	auto hintSize = m_nCheckedSize;
	if(m_status < FIST_DLRECEIVING || m_status > FIST_COMPLETE)
		return -1;

	ldbg("Opening " << m_sPathRel);
	int fd=open(SZABSPATH(m_sPathRel), O_RDONLY);
	
#ifdef HAVE_FADVISE
	// optional, experimental
	if(fd>=0)
		posix_fadvise(fd, 0, hintSize, POSIX_FADV_SEQUENTIAL);
#endif

	return fd;
}

off_t GetFileSize(cmstring & path, off_t defret)
{
	struct stat stbuf;
	return (0==::stat(path.c_str(), &stbuf)) ? stbuf.st_size : defret;
}

/*
void fileitem::ResetCacheState()
{
	setLockGuard;
	m_nSizeSeenInCache = 0;
	m_nUsableSizeInCache = 0;
	m_status = FIST_FRESH;
	m_bAllowStoreData = true;
	m_head.clear();
}
*/

fileitem::FiStatus fileitem::SetupFromCache(bool bCheckFreshness)
{
	LOGSTART2("fileitem::Setup", bCheckFreshness);

	{
		lockguard g(m_mx);
		auto xpected = FIST_FRESH;
		auto ourTurn = m_status == FIST_FRESH;
		if(!ourTurn) //		.compare_exchange_strong(xpected, FIST_INITIALIZING);
			return xpected;
		m_status = FIST_INITIALIZING;
	}

	cmstring sPathAbs(CACHE_BASE+m_sPathRel);

	auto error_clean = [this, &sPathAbs]() -> FiStatus
	{
		unlink( (LPCSTR) (sPathAbs+".head").c_str());
		m_head.clear();
		m_nSizeSeenInCache=0;
		m_status=FIST_INITED;
		return m_status;
	};

	// now we are in the caller thread's turf, can do IO in critical section
	lockguard g(m_mx);

	m_bCheckFreshness = bCheckFreshness;
	

	if(m_head.LoadFromFile(sPathAbs+".head") >0 && m_head.type==header::ANSWER )
	{
		if(200 != m_head.getStatus())
			return error_clean();

		LOG("good head");

		m_nSizeSeenInCache=GetFileSize(sPathAbs, 0);

		// some plausibility checks
		if(m_bCheckFreshness)
		{
			const char *p=m_head.h[header::LAST_MODIFIED];
			if(!p)
				return error_clean(); // suspicious, cannot use it
			LOG("check freshness, last modified: " << p );

			// that will cause check by if-mo-only later, needs to be sure about the size here
			if(cfg::vrangeops == 0
					&& m_nSizeSeenInCache != atoofft(m_head.h[header::CONTENT_LENGTH], -17))
			{
				m_nSizeSeenInCache = 0;
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
				if(nContLen < m_nSizeSeenInCache)
					return error_clean();

				LOG("Content-Length has a sane range");
				
				m_nCheckedSize = m_nSizeSeenInCache;

				// is it complete? and 0 value also looks weird, try to verify later
				if(m_nSizeSeenInCache == nContLen && nContLen>0)
					m_status=FIST_COMPLETE;
			}
			else
			{
				// no content length known, assume it's ok
				m_nCheckedSize=m_nSizeSeenInCache;
			}
		}
	}
	else // -> no .head file
	{
		// maybe there is some left-over without head file?
		// Don't thrust volatile data, but otherwise try to reuse?
		if(!bCheckFreshness)
			m_nSizeSeenInCache=GetFileSize(sPathAbs, 0);
	}
	LOG("resulting status: " << (int) m_status);
	if(m_status < FIST_INITED)
		m_status = FIST_INITED;
	return m_status;
}

/*
bool fileitem::CheckUsableRange_unlocked(off_t nRangeLastByte)
{
	if(m_status == FIST_COMPLETE)
		return true;
	if(m_status < FIST_INITED || m_status > FIST_COMPLETE)
		return false;
	if(m_status >= FIST_DLGOTHEAD)
		return nRangeLastByte > m_nUsableSizeInCache;

	// special exceptions for static files
	return (m_status == FIST_INITED && !m_bCheckFreshness
			&& m_nSizeSeenInCache>0 && nRangeLastByte >=0 && nRangeLastByte <m_nSizeSeenInCache
			&& atoofft(m_head.h[header::CONTENT_LENGTH], -255) > nRangeLastByte);
}
*/

bool fileitem::ResetCacheState(bool bForce)
{
	lockguard g(m_mx);

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
	cmstring sPathAbs(SABSPATH(m_sPathRel));
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
	m_nSizeSeenInCache=m_nCheckedSize=0;

	return true;
}

void fileitem::SetComplete()
{
	lockguard g(m_mx);
	m_nCheckedSize = m_nSizeSeenInCache;
	m_status = FIST_COMPLETE;
	m_cvState.notify_all();
	notifyObserversNoLock();
}

#warning lock, access?
void fileitem::UpdateHeadTimestamp()
{
	{
		lockguard g(m_mx);
		if(m_status < FIST_DLGOTHEAD || m_status > FIST_COMPLETE || m_sPathRel.empty())
			return;
	}
	utimes(SZABSPATHSFX(m_sPathRel, sHead), nullptr);
}
/*
fileitem::FiStatus fileitem::WaitForFinish(int *httpCode)
{
	lockuniq g(this);
	while(m_status<FIST_COMPLETE)
		wait(g);
	if(httpCode)
		*httpCode=m_head.getStatus();
	return m_status;
}
*/
inline void _LogWithErrno(cmstring& msg, const string & sFile)
{
	tErrnoFmter f;
	log::err(tSS() << sFile <<
			" storage error [" << msg << "], last errno: " << f);
}

#ifndef MINIBUILD

bool tFileItemEx::DownloadStartedStoreHeader(const header & h, size_t hDataLen,
		const char *pNextData,
		bool bRestartResume, bool &bDoCleanRetry)
{
	LOGSTART("fileitem::DownloadStartedStoreHeader");

	// big critical section is ok since all the waiters have to get the state soon anyhow
	lockguard g(m_mx);

	auto SETERROR = [this, &h](cmstring& x) {
		m_bAllowStoreData=false;
		{
			m_head.frontLine=mstring("HTTP/1.1 ")+x;
			m_head.set(header::XORIG, h.h[header::XORIG]);
			m_status=FIST_DLERROR;
			m_cvState.notify_all();
		}
		_LogWithErrno(x, m_sPathRel);
	};

	auto withError = [this, &SETERROR](cmstring& x) {
		SETERROR(x);
		return false;
	};
	cmstring sPathAbs(CACHE_BASE+m_sPathRel);
	string sHeadPath=sPathAbs + ".head";

	auto withErrorAndKillFile = [this, &SETERROR, &sPathAbs, &sHeadPath](LPCSTR x)
	{
		SETERROR(x);
		if(m_filefd>=0)
		{
#if _POSIX_SYNCHRONIZED_IO > 0
			fsync(m_filefd);
#endif
			forceclose(m_filefd);
		}

		//LOG("Deleting " << sPathAbs);
		::unlink(sPathAbs.c_str());
		::unlink(sHeadPath.c_str());

		m_status=FIST_DLERROR;
		return false;
	};

	USRDBG( "Download started, storeHeader for " << m_sPathRel << ", current status: " << (int) m_status);

	// always news for someone, even when lost the race
	auto notifAction = [this]() {
		// nope, we have a lock already... notifyObservers();
		for(const auto& n: m_subscribers)
			if(n != -1)
				poke(n);
		m_cvState.notify_all();
	};
	ACTION_ON_LEAVING_EX(_notifier, notifAction)

	// legal "transitions"
	// normally from inited to assigned
	// for hot restart and volatile:

	bool bStateEntered = false;
	auto xpect = FIST_INITED;
	if(!bRestartResume)
	{
		bStateEntered = m_status == xpect;
		if(bStateEntered)
			m_status= FIST_DLASSIGNED;
	}
	else if(m_bCheckFreshness) // can only resume from assigned, anything else would be crap
	{
		if(m_nCheckedSize != 0)
			return false; // just to be sure, don't report bad data in any case
		bStateEntered = m_status == FIST_DLASSIGNED;
	}
	else // non-volatile files...
	{
		if(m_status < FIST_DLASSIGNED)
			m_status = FIST_DLASSIGNED;
		else
			bStateEntered = m_status < FIST_COMPLETE;
		// and will check resumed position below
	}
	if(!bStateEntered)
		return false;

	if(m_status == FIST_DLASSIGNED)
		m_dlThreadId = pthread_self();

	// optional optimisation: hints for the filesystem resp. kernel
	off_t hint_start(0), hint_length(0);
	

	int serverStatus = h.getStatus();
	switch(serverStatus)
	{
	case 200:
	{
		if(m_status < FIST_DLGOTHEAD)
			bRestartResume = false; // behave normally, set all data

		if (bRestartResume)
		{
			if (m_nCheckedSize != 0)
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
			m_nCheckedSize=0;
			m_head=h;
		}
		hint_length=atoofft(h.h[header::CONTENT_LENGTH], 0);
		break;
	}
	case 206:
	{

		if(m_nSizeSeenInCache<=0 && m_nRangeLimit<0)
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
				|| (m_nSizeSeenInCache>0 && myfrom != m_nSizeSeenInCache-1)
				|| (m_nRangeLimit>=0 && myto != m_nRangeLimit)
				|| myfrom<0 || mylen<0
		)
		{
			return withError("500 Server reports unexpected range");
		}

		m_nCheckedSize=myfrom;
		
		hint_start=myfrom;
		hint_length=mylen;

		m_head=h;
		m_head.frontLine="HTTP/1.1 200 OK";
		m_head.del(header::CONTENT_RANGE);
		m_head.set(header::CONTENT_LENGTH, mylen);
		m_head.set(header::XORIG, h.h[header::XORIG]);

		// target opened before? close it, will reopen&seek later
		if (bRestartResume)
			checkforceclose(m_filefd);

		// special optimisation; if "-1 trick" was used then maybe don't reopen that file for writing later
		if(m_bCheckFreshness && pNextData && m_nSizeSeenInCache == mylen && m_nCheckedSize == mylen-1)
		{
			int fd=open(sPathAbs.c_str(), O_RDONLY);
			if(fd>=0)
			{
				if(m_nCheckedSize==lseek(fd, m_nCheckedSize, SEEK_SET))
				{
					char c;
					if(1 == read(fd, &c, 1) && c == *pNextData)
					{
						if(cfg::debug & log::LOG_DEBUG)
							log::err(tSS() << "known data hit, don't write to: "<< m_sPathRel);
						m_bAllowStoreData=false;
						m_nCheckedSize=mylen;
					}
				}
				// XXX: optimize that, open as RW if possible and keep the file open for writing
				forceclose(fd);
			}
		}
		break;
	}
	case 416:
		// that's bad; it cannot have been completed before (the -1 trick)
		// however, proxy servers with v3r4 cl3v3r caching strategy can cause that
		// if if-mo-since is used and they don't like it, so attempt a retry in this case
		if(m_nCheckedSize == 0)
		{
			USRDBG( "Peer denied to resume previous download (transient error) " << m_sPathRel );
			m_nSizeSeenInCache = 0;
			bDoCleanRetry=true;
			_notifier.defuse();
			return false;
		}
		else if(m_bIsGloballyRegistered) // only cleanup when acting on cache
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
		if(bRestartResume)
		{
			// got an error from the replacement mirror? cannot handle it properly
			// because some job might already have started returning the data
			USRDBG( "Cannot restart, HTTP code: " << serverStatus);
			return withError(h.getCodeMessage());
		}

		m_bAllowStoreData=false;
		// have a clean header with just the error message
		m_head.frontLine=h.frontLine;
		m_head.set(header::CONTENT_LENGTH, "0");
		if(m_status>FIST_DLGOTHEAD && m_bIsGloballyRegistered) // only enable when acting on cache
		{
			// oh shit. Client may have already started sending it. Prevent such trouble in future.
			unlink(sHeadPath.c_str());
		}
	}

	if(cfg::debug & log::LOG_MORE)
		log::misc(string("Download of ")+m_sPathRel+" started");

	if(m_bAllowStoreData)
	{
		// using adaptive Delete-Or-Replace-Or-CopyOnWrite strategy

		MoveRelease2Sidestore();

		// First opening the file to be sure that it can be written. Header storage is the critical point,
		// every error after that leads to full cleanup to not risk inconsistent file contents 
		
		int flags = O_WRONLY | O_CREAT | O_BINARY;
		struct stat stbuf;
				
		mkbasedir(sPathAbs);

		m_filefd=open(sPathAbs.c_str(), flags, cfg::fileperms);
		ldbg("file opened?! returned: " << m_filefd);
		
		// self-recovery from cache poisoned with files with wrong permissions
		if (m_filefd<0)
		{
			if(m_nCheckedSize>0) // OOOH CRAP! CANNOT APPEND HERE! Do what's still possible.
			{
				string temp=sPathAbs+".tmp";
				if(FileCopy(sPathAbs, temp) && 0==unlink(sPathAbs.c_str()) )
				{
					if(0!=rename(temp.c_str(), sPathAbs.c_str()))
						return withError("503 Cannot rename files");
					
					// be sure about that
					if(0!=stat(sPathAbs.c_str(), &stbuf) || stbuf.st_size!=m_nSizeSeenInCache)
						return withError("503 Cannot copy file parts, filesystem full?");
					
					m_filefd=open(sPathAbs.c_str(), flags, cfg::fileperms);
					ldbg("file opened after copying around: ");
				}
				else
					return withError((tSS()<<"503 Cannot store or remove files in "
							<< GetDirPart(sPathAbs)).c_str());
			}
			else
			{
				unlink(sPathAbs.c_str());
				m_filefd=open(sPathAbs.c_str(), flags, cfg::fileperms);
				ldbg("file force-opened?! returned: " << m_filefd);
			}
		}
		
		if (m_filefd<0)
		{
			tErrnoFmter efmt("503 Cache storage error - ");
#ifdef DEBUG
			return withError((efmt+sPathAbs).c_str());
#else
			return withError(efmt.c_str());
#endif
		}
		
		if(0!=fstat(m_filefd, &stbuf) || !S_ISREG(stbuf.st_mode))
			return withErrorAndKillFile("503 Not a regular file");

		// this makes sure not to truncate file while it's mmaped
		auto tempLock = TFileShrinkGuard::Acquire(stbuf);
		
		// crop, but only if the new size is smaller. MUST NEVER become larger (would fill with zeros)
		if(m_nCheckedSize <= m_nSizeSeenInCache)
		{
			if(0==ftruncate(m_filefd, m_nCheckedSize))
      {
#if ( (_POSIX_C_SOURCE >= 199309L || _XOPEN_SOURCE >= 500) && _POSIX_SYNCHRONIZED_IO > 0)
				fdatasync(m_filefd);
#endif
      }
			else
				return withErrorAndKillFile("503 Cannot change file size");
		}
		else if(m_nCheckedSize>m_nSizeSeenInCache) // should never happen and caught by the checks above
			return withErrorAndKillFile("503 Internal error on size checking");
		// else... nothing to fix since the expectation==reality

		falloc_helper(m_filefd, hint_start, hint_length);
    /*
     * double-check the docs, this is probably not relevant for writting
#ifdef HAVE_FADVISE
	// optional, experimental
		posix_fadvise(m_filefd, hint_start, hint_length, POSIX_FADV_SEQUENTIAL);
#endif
*/
		ldbg("Storing header as "+sHeadPath);
		int count=m_head.StoreToFile(sHeadPath);

		if(count<0)
			return withErrorAndKillFile( (-count!=ENOSPC
					? "503 Cache storage error" : "503 OUT OF DISK SPACE"));
			
		// double-check the sane state
		if(0!=fstat(m_filefd, &stbuf) || stbuf.st_size!=m_nCheckedSize)
			return withErrorAndKillFile("503 Inconsistent file state");
			
		if(m_nCheckedSize!=lseek(m_filefd, m_nCheckedSize, SEEK_SET))
			return withErrorAndKillFile("503 IO error, positioning");
	}
	
	m_status=FIST_DLGOTHEAD;
	return true;
}

bool tFileItemEx::StoreFileData(const char *data, unsigned int size)
{
	LOGSTART2("fileitem::StoreFileData", "status: " <<  (int) m_status << ", size: " << size);

	// lock carefully to avoid pointless critical sections, especially around IO

	off_t tgtsize;
	{
		lockguard g(m_mx);
		if(m_status < FIST_DLGOTHEAD || m_status > FIST_DLRECEIVING)
			return false;
		tgtsize = m_nCheckedSize + size;
		if(size)
			m_status = FIST_DLRECEIVING;
	}
	
	if (size==0)
	{
		if (cfg::debug & log::LOG_MORE)
			log::misc(tSS() << "Download of " << m_sPathRel << " finished");
		m_head.StoreToFile(SZABSPATHSFX(m_sPathRel, sHead));
		lockguard g(m_mx);
		// we are done! Fix header from chunked transfers?
		if (m_filefd >= 0 && !m_head.h[header::CONTENT_LENGTH])
			m_head.set(header::CONTENT_LENGTH, m_nCheckedSize);
		m_status = FIST_COMPLETE;
		notifyObserversNoLock();
		m_cvState.notify_all();
	}
	else
	{
		if (m_bAllowStoreData && m_filefd>=0)
		{
			while(size>0)
			{
				int r=write(m_filefd, data, size);
				if(r<0)
				{
					if(EINTR==errno || EAGAIN==errno)
						continue;

					tErrnoFmter efmt("HTTP/1.1 503 ");
					lockguard g(m_mx);
					m_head.frontLine = efmt;
					m_status = FIST_DLERROR;
					// message will be set by the caller
					_LogWithErrno(efmt.c_str(), m_sPathRel);
					notifyObserversNoLock();
					m_cvState.notify_all();
					return false;
				}
				size-=r;
				data+=r;
			}
		}
	}

	lockguard g(m_mx);
	m_nCheckedSize = tgtsize;
	// needs to remember the good size, just in case the DL is resumed (restarted in hot state)
	if(m_nSizeSeenInCache < m_nCheckedSize)
		m_nSizeSeenInCache = m_nCheckedSize;
	notifyObserversNoLock();
	// NOT pinging state watchers
	return true;
}

tFileItemPtr tFileItemEx::CreateRegistered(cmstring& sPathUnescaped,
		const tFileItemPtr& existingFi, bool *created4caller)
{
	mstring sPathRel(tFileItemEx::NormalizePath(sPathUnescaped));
	tFileItemPtr ret;
	if(created4caller) *created4caller = false;
	{
		lockguard g(g_sharedItemsMx);
#warning also do house keeping in the stickyItemCache prio-q
		auto itWeak = g_sharedItems.find(sPathRel);
		if(itWeak != g_sharedItems.end())
			ret = itWeak->second.lock();
		if(ret && existingFi && ret != existingFi) // conflict, report failure
			return tFileItemPtr();
		if(!ret)
		{
			ret = std::make_shared<tFileItemEx>(sPathRel);
			EMPLACE_PAIR_COMPAT(g_sharedItems, sPathRel, ret);
			ret->m_bIsGloballyRegistered = true;
			if(created4caller) *created4caller = true;
		}
	}
	return ret;
}

bool tFileItemEx::TryDispose(tFileItemPtr& existingFi)
{
	lockguard g(g_sharedItemsMx);
	if(!existingFi || existingFi.use_count()>1)
		return false; // cannot dispose, invalid or still used by others than the caller
	auto it = g_sharedItems.find(existingFi->m_sPathRel);
	if(it == g_sharedItems.end())
		return false; // weird
	auto sptr = it->second.lock();
	if(sptr != existingFi) // XXX: report error?
		return false;
	// can do this without looking because no other thread can start sending data ATM
	g_sharedItems.erase(it);
	// nothing should be reading it anymore anyway
	sptr->SetReportStatus(FIST_DLERROR);
	checkforceclose(sptr->m_filefd);
	return true;
}


#if 0
fileItemMgmt::~fileItemMgmt()
{
	LOGSTART("fileItemMgmt::~fileItemMgmt");
	Unreg();
}

inline void fileItemMgmt::Unreg()
{
	LOGSTART("fileItemMgmt::Unreg");

	if(!m_ptr) // unregistered before?
		return;

	lockguard managementLock(mapItemsMx);

	// invalid or not globally registered?
	if(m_ptr->m_globRef == mapItems.end())
		return;

	auto local_ptr(m_ptr); // might disappear
	lockguard fitemLock(*local_ptr);

	if ( -- m_ptr->usercount <= 0)
	{
		if(m_ptr->m_status < fileitem::FIST_COMPLETE && m_ptr->m_status != fileitem::FIST_INITED)
		{
			ldbg("usercount dropped to zero while downloading?: " << (int) m_ptr->m_status);
		}

		// some file items will be held ready for some time
		time_t when(0);
		if (MAXTEMPDELAY && m_ptr->m_bCheckFreshness &&
				m_ptr->m_status == fileitem::FIST_COMPLETE &&
				((when=m_ptr->m_nTimeDlStarted+MAXTEMPDELAY) > GetTime()))
		{
			g_victor.ScheduleFor(when, cleaner::TYPE_EXFILEITEM);
			return;
		}

		// nothing, let's put the item into shutdown state
		m_ptr->m_status = fileitem::FIST_DLSTOP;
		m_ptr->m_head.frontLine="HTTP/1.1 500 Cache file item expired";
		m_ptr->notifyAll();

		LOG("*this is last entry, deleting dl/fi mapping");
		mapItems.erase(m_ptr->m_globRef);
		m_ptr->m_globRef = mapItems.end();

		// make sure it's not double-unregistered accidentally!
		m_ptr.reset();
	}
}


bool fileItemMgmt::PrepareRegisteredFileItemWithStorage(cmstring &sPathUnescaped, bool bConsiderAltStore)
{
	LOGSTART2("fileitem::GetFileItem", sPathUnescaped);

	MYTRY
	{
		mstring sPathRel(tFileItemEx::NormalizePath(sPathUnescaped));
		lockguard lockGlobalMap(mapItemsMx);
		tFiGlobMap::iterator it=mapItems.find(sPathRel);
		if(it!=mapItems.end())
		{
			if (bConsiderAltStore)
			{
				// detect items that got stuck somehow
				time_t now(GetTime());
				time_t extime(now - cfg::stucksecs);
				if (it->second->m_nTimeDlDone < extime)
				{
					// try to find its sibling which is in good state?
					for (; it!=mapItems.end() && it->first == sPathRel; ++it)
					{
						if (it->second->m_nTimeDlDone >= extime)
						{
							it->second->usercount++;
							LOG("Sharing an existing REPLACEMENT file item");
							m_ptr = it->second;
							return true;
						}
					}
					// ok, then create a modded name version in the replacement directory
					mstring sPathRelMod(sPathRel);
					replaceChars(sPathRelMod, "/\\", '_');
					sPathRelMod.insert(0, sReplDir + ltos(getpid()) + "_" + ltos(now)+"_");
					LOG("Registering a new REPLACEMENT file item...");
					auto sp(make_shared<tFileItemEx>(sPathRelMod, 1));
					sp->m_globRef = mapItems.insert(make_pair(sPathRel, sp));
					m_ptr = sp;
					return true;
				}
			}
			LOG("Sharing existing file item");
			it->second->usercount++;
			m_ptr = it->second;
			return true;
		}
		LOG("Registering the NEW file item...");
		auto sp(make_shared<tFileItemEx>(sPathRel, 1));
		sp->m_globRef = mapItems.insert(make_pair(sPathRel, sp));
		//lockGlobalMap.unLock();
		m_ptr = sp;
		return true;
	}
	MYCATCH(std::bad_alloc&)
	{
	}
	return false;
}

// make the fileitem globally accessible
bool fileItemMgmt::RegisterFileItem(tFileItemPtr spCustomFileItem)
{
	LOGSTART2("fileitem::RegisterFileItem", spCustomFileItem->m_sPathRel);

	if (!spCustomFileItem || spCustomFileItem->m_sPathRel.empty())
		return false;

	Unreg();

	lockguard lockGlobalMap(mapItemsMx);

	if(ContHas(mapItems, spCustomFileItem->m_sPathRel))
		return false; // conflict, another agent is already active

	spCustomFileItem->m_globRef = mapItems.emplace(spCustomFileItem->m_sPathRel,
			spCustomFileItem);
	spCustomFileItem->usercount=1;
	m_ptr = spCustomFileItem;
	return true;
}

void fileItemMgmt::RegisterFileitemLocalOnly(fileitem* replacement)
{
	LOGSTART2("fileItemMgmt::ReplaceWithLocal", replacement);
	Unreg();
	m_ptr.reset(replacement);
}


// this method is supposed to be awaken periodically and detects items with ref count manipulated by
// the request storm prevention mechanism. Items shall be be dropped after some time if no other
// thread but us is using them.
time_t fileItemMgmt::BackgroundCleanup()
{
	LOGSTART2s("fileItemMgmt::BackgroundCleanup", GetTime());
	lockguard lockGlobalMap(mapItemsMx);
	tFiGlobMap::iterator it, here;

	time_t now=GetTime();
	time_t oldestGet = END_OF_TIME;
	time_t expBefore = now - MAXTEMPDELAY;

	for(it=mapItems.begin(); it!=mapItems.end();)
	{
		here=it++;

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

	// preserving a few seconds to catch more of them in the subsequent run
	return std::max(oldestGet + MAXTEMPDELAY, GetTime()+8);
}
#endif

ssize_t tFileItemEx::SendData(int out_fd, int in_fd, off_t &nSendPos, size_t count)
{
#ifndef HAVE_LINUX_SENDFILE
	return sendfile_generic(out_fd, in_fd, &nSendPos, count);
#else
	ssize_t r=sendfile(out_fd, in_fd, &nSendPos, count);

	if(r<0 && (errno==ENOSYS || errno==EINVAL))
		return sendfile_generic(out_fd, in_fd, &nSendPos, count);
	else
		return r;
#endif
}

#if 0
void fileItemMgmt::dump_status()
{
	tSS fmt;
	log::err("File descriptor table:\n");
	for(const auto& item : mapItems)
	{
		fmt.clear();
		fmt << "FREF: " << item.first << " [" << item.second->usercount << "]:\n";
		if(! item.second)
		{
			fmt << "\tBAD REF!\n";
			continue;
		}
		else
		{
			fmt << "\t" << item.second->m_sPathRel
					<< "\n\tDlRefCount: " << item.second->m_nDlRefsCount
					<< "\n\tState: " << (int)  item.second->m_status
					<< "\n\tFilePos: " << item.second->m_nIncommingCount << " , "
					<< item.second->m_nRangeLimit << " , "
					<< item.second->m_nCheckedSize << " , "
					<< item.second->m_nSizeSeenInCache
					<< "\n\tGotAt: " << item.second->m_nTimeDlStarted << "\n\n";
		}
		log::err(fmt.c_str(), nullptr);
	}
	log::flush();
}
#endif

tFileItemEx::~tFileItemEx()
{
#if 0
	if(startsWith(m_sPathRel, sReplDir))
	{
		::unlink(SZABSPATH(m_sPathRel));
		::unlink((SABSPATH(m_sPathRel)+".head").c_str());
	}
#endif
	{
		lockguard g(g_sharedItemsMx);

		if(!m_bIsGloballyRegistered)
			return;

		//ASSERT(this == g_sharedItems[m_sPathRel]);
		auto it = g_sharedItems.find(m_sPathRel);
		if (it != g_sharedItems.end())
		{
			auto backRef = it->second.lock();
			if (backRef) // this was ok, release the external reference then and we are good
				g_sharedItems.erase(m_sPathRel);
			// else... what? set some tag to become harmless?
		}
	}

}

// keep a snapshot of that files for later identification of by-hash-named metadata
int tFileItemEx::MoveRelease2Sidestore()
{
	if(m_nCheckedSize)
		return 0;
	if(!endsWithSzAr(m_sPathRel, "/InRelease") && !endsWithSzAr(m_sPathRel, "/Release"))
		return 0;
	cmstring tgtDir = CACHE_BASE + cfg::privStoreRelSnapSufix + sPathSep + GetDirPart(m_sPathRel);
	mkdirhier(tgtDir);
	cmstring srcAbs = CACHE_BASE + m_sPathRel;
	Cstat st(srcAbs);
	auto sideFileAbs = tgtDir + ltos(st.st_ino) + ltos(st.st_mtim.tv_sec) + ltos(st.st_mtim.tv_nsec);
	return FileCopy(srcAbs, sideFileAbs);
	//return rename(srcAbs.c_str(), sideFileAbs.c_str());
}

#endif // MINIBUILD

void fileitem::poke(int fd)
{
#ifdef HAVE_LINUX_EVENTFD
       while(eventfd_write(fd, 1)<0) ;
#else
       POKE(fd);
#endif
}

void fileitem::subscribe(int fd)
{
	ASSERT(fd != -1);
	lockguard g(m_mx);
	m_subscribers.push_back(fd);
	// might have changed since lock was issued
	poke(fd);
}

void fileitem::unsubscribe(int fd)
{
	lguard g(m_mx);
	for(auto& m: m_subscribers)
		if(m == fd)
			m = -1;
}

void fileitem::notifyObservers()
{
	lguard g(m_mx);
	for(const auto& n: m_subscribers)
		if(n != -1)
			poke(n);
}


void fileitem::notifyObserversNoLock()
{
	for(const auto& n: m_subscribers)
		if(n != -1)
			poke(n);
}

#warning sErrorMsg filled out correctly and reliable?

fileitem::FiStatus fileitem::GetStatus(mstring* sErrorMsg,
		off_t* pnConfirmedSizeSoFar, header* retHead)
{
	lguard g(m_mx);
//	if(dlThreadId && m_status >= FIST_DLASSIGNED)
//		*dlThreadId = m_dlThreadId;
	if(sErrorMsg)
		sErrorMsg->assign(m_head.getCodeMessage());
	if(pnConfirmedSizeSoFar)
		*pnConfirmedSizeSoFar = m_nCheckedSize;
	if(retHead)
		*retHead = m_head;
	return m_status;
}

#warning implement this and use it again (subscribe for updates on fitem inside and... wait for >= complete)

fileitem::FiStatus fileitem::WaitForFinish(int* httpCode)
{
	ulock g(m_mx);
	while(m_status < FIST_COMPLETE)
		m_cvState.wait(g);
	if(httpCode)
		*httpCode = m_head.getStatus();
	return m_status;
}

void fileitem::SetReportStatus(FiStatus fistat)
{
	lguard g(m_mx);
	m_status = fistat;
	m_cvState.notify_all();
}

}
