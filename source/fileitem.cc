
//#define LOCAL_DEBUG
#include "debug.h"

#include "config.h"
#include "fileitem.h"
#include "header.h"
#include "acfg.h"
#include "acbuf.h"
#include "fileio.h"
#include "cleaner.h"

#include <errno.h>
#include <algorithm>

using namespace std;

#define MAXTEMPDELAY acfg::maxtempdelay // 27
mstring sReplDir("_altStore" SZPATHSEP);

static tFiGlobMap mapItems;

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

fileitem::fileitem() :
	condition(),
	m_nIncommingCount(0),
	m_nSizeSeen(0),
	m_nRangeLimit(-1),
	m_bCheckFreshness(true),
	m_bHeadOnly(false),
	m_bAllowStoreData(true),
	m_nSizeChecked(0),
	m_filefd(-1),
	m_nDlRefsCount(0),
	m_status(FIST_FRESH),
	m_nTimeDlStarted(0),
	m_nTimeDlDone(END_OF_TIME),
	m_globRef(mapItems.end()),
	usercount(0)
{
}

fileitem::~fileitem()
{
	//setLockGuard;
//	m_head.clear();
	checkforceclose(m_filefd);
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

		if (acfg::debug&LOG_MORE)
			aclog::misc(string("Download of ")+m_sPathRel+" aborted");
	}
	checkforceclose(m_filefd);
}

uint64_t fileitem::GetTransferCount()
{
	setLockGuard;
	uint64_t ret=m_nIncommingCount;
	m_nIncommingCount=0;
	return ret;
}

int fileitem::GetFileFd() {
	LOGSTART("fileitem::GetFileFd");
	setLockGuard;

	ldbg("Opening " << m_sPathRel);
	int fd=open(SZABSPATH(m_sPathRel), O_RDONLY);
	
#ifdef HAVE_FADVISE
	// optional, experimental
	if(fd>=0)
		posix_fadvise(fd, 0, m_nSizeChecked, POSIX_FADV_SEQUENTIAL);
#endif

	return fd;
}

off_t GetFileSize(cmstring & path, off_t defret)
{
	struct stat stbuf;
	return (0==::stat(path.c_str(), &stbuf)) ? stbuf.st_size : defret;
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
	
	cmstring sPathAbs(CACHE_BASE+m_sPathRel);

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
			if(acfg::vrangeops == 0
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
	LOG("resulting status: " << m_status);
	return m_status;

	error_clean:
			::unlink((sPathAbs+".head").c_str());
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
	cmstring sPathAbs(SABSPATH(m_sPathRel));
	if(::truncate(sPathAbs.c_str(), 0) || ::truncate((sPathAbs+".head").c_str(), 0))
		return false;

	m_head.clear();
	m_nSizeSeen=m_nSizeChecked=0;

	return true;
}

void fileitem::SetupComplete()
{
	setLockGuard;
	notifyAll();
	m_nSizeChecked = m_nSizeSeen;
	m_status = FIST_COMPLETE;
}

fileitem::FiStatus fileitem::WaitForFinish(int *httpCode)
{
	setLockGuard;
	while(m_status<FIST_COMPLETE)
		wait();
	if(httpCode)
		*httpCode=m_head.getStatus();
	return m_status;
}

inline void _LogWithErrno(const char *msg, const string & sFile)
{
	tErrnoFmter f;
	aclog::err(tSS() << sFile <<
			" storage error [" << msg << "], last errno: " << f);
}

#ifndef MINIBUILD

bool fileitem_with_storage::DownloadStartedStoreHeader(const header & h, const char *pNextData,
		bool bForcedRestart, bool &bDoCleanRetry)
{
	LOGSTART("fileitem::DownloadStartedStoreHeader");

	auto SETERROR = [&](LPCSTR x) {
		m_bAllowStoreData=false;
		m_head.frontLine=mstring("HTTP/1.1 ")+x;
		m_head.set(header::XORIG, h.h[header::XORIG]);
	    m_status=FIST_DLERROR; m_nTimeDlDone=GetTime();
		_LogWithErrno(x, m_sPathRel);
	};

	auto withError = [&](LPCSTR x) {
		SETERROR(x);
		return false;
	};

	setLockGuard;

	USRDBG( "Download started, storeHeader for " << m_sPathRel << ", current status: " << m_status);
	
	if(m_status >= FIST_COMPLETE)
	{
		USRDBG( "Download was completed or aborted, not restarting before expiration");
		return false;
	}

	// conflict with another thread's download attempt? Deny, except for a forced restart
	if (m_status > FIST_DLPENDING && !bForcedRestart)
		return false;
	
	if(m_bCheckFreshness)
		m_nTimeDlStarted = GetTime();

	m_nIncommingCount+=h.m_nEstimLength;

	// optional optimization: hints for the filesystem resp. kernel
	off_t hint_start(0), hint_length(0);
	
	// status will change, most likely... ie. return withError action
	notifyAll();

	cmstring sPathAbs(CACHE_BASE+m_sPathRel);
	string sHeadPath=sPathAbs + ".head";

	auto withErrorAndKillFile = [&](LPCSTR x)
	{
		SETERROR(x);
		if(m_filefd>=0)
		{
#if _POSIX_SYNCHRONIZED_IO > 0
			fsync(m_filefd);
#endif
			forceclose(m_filefd);
		}

		LOG("Deleting " << sPathAbs);
		::unlink(sPathAbs.c_str());
		::unlink(sHeadPath.c_str());

		m_status=FIST_DLERROR;
		m_nTimeDlDone=GetTime();
		return false;
	};

	int serverStatus = h.getStatus();
#if 0
#warning FIXME
	static UINT fc=1;
	if(!(++fc % 4))
	{
		serverStatus = 416;
	}
#endif
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
			checkforceclose(m_filefd);

		// special optimization; if "-1 trick" was used then maybe don't reopen that file for writing later
		if(m_bCheckFreshness && pNextData && m_nSizeSeen == mylen && m_nSizeChecked == mylen-1)
		{
			int fd=open(sPathAbs.c_str(), O_RDONLY);
			if(fd>=0)
			{
				if(m_nSizeChecked==lseek(fd, m_nSizeChecked, SEEK_SET))
				{
					char c;
					if(1 == read(fd, &c, 1) && c == *pNextData)
					{
						if(acfg::debug & LOG_DEBUG)
							aclog::err(tSS() << "known data hit, don't write to: "<< m_sPathRel);
						m_bAllowStoreData=false;
						m_nSizeChecked=mylen;
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
		if(m_nSizeChecked == 0)
		{
			USRDBG( "Peer denied to resume previous download (transient error) " << m_sPathRel );
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

		m_bAllowStoreData=false;
		// have a clean header with just the error message
		m_head.frontLine=h.frontLine;
		m_head.set(header::CONTENT_LENGTH, "0");
		if(m_status>FIST_DLGOTHEAD)
		{
			// oh shit. Client may have already started sending it. Prevent such trouble in future.
			unlink(sHeadPath.c_str());
		}
	}

	if(acfg::debug&LOG_MORE)
		aclog::misc(string("Download of ")+m_sPathRel+" started");

	if(m_bAllowStoreData)
	{
		// using adaptive Delete-Or-Replace-Or-CopyOnWrite strategy
		
		// First opening the file first to be sure that it can be written. Header storage is the critical point,
		// every error after that leads to full cleanup to not risk inconsistent file contents 
		
		int flags = O_WRONLY | O_CREAT | O_BINARY;
		struct stat stbuf;
				
		mkbasedir(sPathAbs);
		m_filefd=open(sPathAbs.c_str(), flags, acfg::fileperms);
		ldbg("file opened?! returned: " << m_filefd);
		
		// self-recovery from cache poisoned with files with wrong permissions
		if (m_filefd<0)
		{
			if(m_nSizeChecked>0) // OOOH CRAP! CANNOT APPEND HERE! Do what's still possible.
			{
				string temp=sPathAbs+".tmp";
				if(FileCopy(sPathAbs, temp) && 0==unlink(sPathAbs.c_str()) )
				{
					if(0!=rename(temp.c_str(), sPathAbs.c_str()))
						return withError("503 Cannot rename files");
					
					// be sure about that
					if(0!=stat(sPathAbs.c_str(), &stbuf) || stbuf.st_size!=m_nSizeSeen)
						return withError("503 Cannot copy file parts, filesystem full?");
					
					m_filefd=open(sPathAbs.c_str(), flags, acfg::fileperms);
					ldbg("file opened after copying around: ");
				}
				else
					return withError((tSS()<<"503 Cannot store or remove files in "
							<< GetDirPart(sPathAbs)).c_str());
			}
			else
			{
				unlink(sPathAbs.c_str());
				m_filefd=open(sPathAbs.c_str(), flags, acfg::fileperms);
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
		
		// crop, but only if the new size is smaller. MUST NEVER become larger (would fill with zeros)
		if(m_nSizeChecked <= m_nSizeSeen)
		{
			if(0==ftruncate(m_filefd, m_nSizeChecked))
      {
#if ( (_POSIX_C_SOURCE >= 199309L || _XOPEN_SOURCE >= 500) && _POSIX_SYNCHRONIZED_IO > 0)
				fdatasync(m_filefd);
#endif
      }
			else
				return withErrorAndKillFile("503 Cannot change file size");
		}
		else if(m_nSizeChecked>m_nSizeSeen) // should never happen and caught by the checks above
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
			return withErrorAndKillFile( (-count!=ENOSPC ? "503 Cache storage error" : "503 OUT OF DISK SPACE"));
			
		// double-check the sane state
		if(0!=fstat(m_filefd, &stbuf) || stbuf.st_size!=m_nSizeChecked)
			return withErrorAndKillFile("503 Inconsistent file state");
			
		if(m_nSizeChecked!=lseek(m_filefd, m_nSizeChecked, SEEK_SET))
			return withErrorAndKillFile("503 IO error, positioning");
	}
	
	m_status=FIST_DLGOTHEAD;
	return true;
}

bool fileitem_with_storage::StoreFileData(const char *data, unsigned int size)
{
	setLockGuard;

	LOGSTART2("fileitem::StoreFileData", "status: " << m_status << ", size: " << size);

	// something might care, most likely... also about BOUNCE action
	notifyAll();

	m_nIncommingCount+=size;
	
	if(m_status > FIST_COMPLETE || m_status < FIST_DLGOTHEAD)
	{
		ldbg("StoreFileData rejected, status: " << m_status)
		return false;
	}
	
	if (size==0)
	{
		if(FIST_COMPLETE == m_status)
		{
			LOG("already completed");
		}
		else
		{
			m_status = FIST_COMPLETE;
			m_nTimeDlDone=GetTime();

			if (acfg::debug & LOG_MORE)
				aclog::misc(tSS() << "Download of " << m_sPathRel << " finished");

			// we are done! Fix header from chunked transfers?
			if (m_filefd >= 0 && !m_head.h[header::CONTENT_LENGTH])
			{
				m_head.set(header::CONTENT_LENGTH, m_nSizeChecked);
				m_head.StoreToFile(CACHE_BASE + m_sPathRel + ".head");
			}
		}
	}
	else
	{
		m_status = FIST_DLRECEIVING;

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
					m_head.frontLine = efmt;
					m_status=FIST_DLERROR;
					// message will be set by the caller
					m_nTimeDlDone=GetTime();
					_LogWithErrno(efmt.c_str(), m_sPathRel);
					return false;
				}
				m_nSizeChecked+=r;
				size-=r;
				data+=r;
			}
		}
	}

	// needs to remember the good size, just in case the DL is resumed (restarted in hot state)
	if(m_nSizeSeen < m_nSizeChecked)
		m_nSizeSeen = m_nSizeChecked;

	return true;
}

fileItemMgmt::~fileItemMgmt()
{
	LOGSTART("fileItemMgmt::~fileItemMgmt");
	Unreg();
}

inline void fileItemMgmt::Unreg()
{
	LOGSTART("fileItemMgmt::Unreg");

	if(!m_ptr)
		return;

	lockguard managementLock(mapItems);

	// invalid or not globally registered?
	if(m_ptr->m_globRef == mapItems.end())
		return;

	lockguard fitemLock(*m_ptr);

	if ( -- m_ptr->usercount <= 0)
	{
		if(m_ptr->m_status < fileitem::FIST_COMPLETE && m_ptr->m_status != fileitem::FIST_INITED)
		{
			ldbg("usercount dropped to zero while downloading?: " << m_ptr->m_status);
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
	}
}


fileItemMgmt fileItemMgmt::GetRegisteredFileItem(cmstring &sPathUnescaped, bool bConsiderAltStore)
{
	LOGSTART2s("fileitem::GetFileItem", sPathUnescaped);

	MYTRY
	{
		mstring sPathRel(fileitem_with_storage::NormalizePath(sPathUnescaped));
		lockguard lockGlobalMap(mapItems);
		tFiGlobMap::iterator it=mapItems.find(sPathRel);
		if(it!=mapItems.end())
		{
			if (bConsiderAltStore)
			{
				// detect items that got stuck somehow
				time_t now(GetTime());
				time_t extime(now - acfg::stucksecs);
				if (it->second->m_nTimeDlDone < extime)
				{
					// try to find its sibling which is in good state?
					for (; it!=mapItems.end() && it->first == sPathRel; ++it)
					{
						if (it->second->m_nTimeDlDone >= extime)
						{
							it->second->usercount++;
							LOG("Sharing an existing REPLACEMENT file item");
							fileItemMgmt ret;
							ret.m_ptr = it->second;
							return ret;
						}
					}
					// ok, then create a modded name version in the replacement directory
					mstring sPathRelMod(sPathRel);
					replaceChars(sPathRelMod, "/\\", '_');
					sPathRelMod.insert(0, sReplDir + ltos(getpid()) + "_" + ltos(now)+"_");
					LOG("Registering a new REPLACEMENT file item...");
					fileitem_with_storage *p = new fileitem_with_storage(sPathRelMod);
					p->usercount=1;
					tFileItemPtr sp(p);
					p->m_globRef = mapItems.insert(make_pair(sPathRel, sp));
					fileItemMgmt ret;
					ret.m_ptr = sp;
					return ret;
				}
			}
			LOG("Sharing existing file item");
			it->second->usercount++;
			fileItemMgmt ret;
			ret.m_ptr = it->second;
			return ret;
		}
		LOG("Registering the NEW file item...");
		fileitem_with_storage *p = new fileitem_with_storage(sPathRel);
		p->usercount=1;
		tFileItemPtr sp(p);
		p->m_globRef = mapItems.insert(make_pair(sPathRel, sp));
		//lockGlobalMap.unLock();
		fileItemMgmt ret;
		ret.m_ptr = sp;
		return ret;
	}
	MYCATCH(std::bad_alloc&)
	{
	}
	return fileItemMgmt();
}

// make the fileitem globally accessible
fileItemMgmt fileItemMgmt::RegisterFileItem(tFileItemPtr spCustomFileItem)
{
	if (!spCustomFileItem || spCustomFileItem->m_sPathRel.empty())
		return fileItemMgmt();

	lockguard lockGlobalMap(mapItems);

	if(ContHas(mapItems, spCustomFileItem->m_sPathRel))
		return fileItemMgmt(); // conflict, another agent is already active

	spCustomFileItem->m_globRef = mapItems.insert(make_pair(spCustomFileItem->m_sPathRel,
			spCustomFileItem));

	spCustomFileItem->usercount=1;
	fileItemMgmt ret;
	ret.m_ptr = spCustomFileItem;
	return ret;
}


// this method is supposed to be awaken periodically and detect items with ref count manipulated by
// the request storm prevention mechanism. Items shall be be dropped after some time if no other
// thread but us is using them.
time_t fileItemMgmt::BackgroundCleanup()
{
	LOGSTART2s("fileItemMgmt::BackgroundCleanup", GetTime());
	lockguard lockGlobalMap(mapItems);
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

ssize_t fileitem_with_storage::SendData(int out_fd, int in_fd, off_t &nSendPos, size_t count)
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

void fileItemMgmt::dump_status()
{
	tSS fmt;
	aclog::err("File descriptor table:\n");
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
					<< "\n\tState: " << item.second->m_status
					<< "\n\tFilePos: " << item.second->m_nIncommingCount << " , "
					<< item.second->m_nRangeLimit << " , "
					<< item.second->m_nSizeChecked << " , "
					<< item.second->m_nSizeSeen
					<< "\n\tGotAt: " << item.second->m_nTimeDlStarted << "\n\n";
		}
		aclog::err(fmt.c_str(), NULL);
	}
	aclog::flush();
}

fileitem_with_storage::~fileitem_with_storage()
{
	if(startsWith(m_sPathRel, sReplDir))
	{
		::unlink(SZABSPATH(m_sPathRel));
		::unlink((SABSPATH(m_sPathRel)+".head").c_str());
	}
}

fileItemMgmt::fileItemMgmt(const fileItemMgmt &src)
{
	fileItemMgmt *x = const_cast<fileItemMgmt *>(&src);
	this->m_ptr = x->m_ptr;
	x->m_ptr.reset();
}

fileItemMgmt& fileItemMgmt::operator=(const fileItemMgmt &src)
{
	fileItemMgmt *x = const_cast<fileItemMgmt *>(&src);
	this->m_ptr = x->m_ptr;
	x->m_ptr.reset();
	return *this;
}

void fileItemMgmt::ReplaceWithLocal(fileitem* replacement)
{
	Unreg();
	m_ptr.reset(replacement);
}

#endif // MINIBUILD
