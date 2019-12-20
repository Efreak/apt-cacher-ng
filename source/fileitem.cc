
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

#include <errno.h>
#include <algorithm>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>

using namespace std;

namespace acng
{
#define MAXTEMPDELAY acng::cfg::maxtempdelay // 27

static tFiGlobMap mapItems;
#ifndef MINIBUILD
static acmutex mapItemsMx;
#endif

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
			m_nIncommingCount(0),
			m_nSizeSeen(0),
			m_nRangeLimit(-1),
			m_bCheckFreshness(true),
			m_bHeadOnly(false),
			m_bAllowStoreData(true),
			m_nSizeChecked(0),
			m_filefd(-1),
			m_nDlRefsCount(0),
			usercount(0),
			m_status(FIST_FRESH),
			m_nTimeDlStarted(0),
			m_nTimeDlDone(END_OF_TIME),
			m_globRef(mapItems.end())
{
}

fileitem::~fileitem()
{
	//setLockGuard;
	//	m_head.clear();
	Truncate2checkedSize();
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

		if (cfg::debug&log::LOG_MORE)
			log::misc(string("Download of ")+m_sPathRel+" aborted");
	}
	Truncate2checkedSize();
	checkforceclose(m_filefd);
}

uint64_t fileitem::GetTransferCount()
{
	setLockGuard;
	uint64_t ret=m_nIncommingCount;
	m_nIncommingCount=0;
	return ret;
}

unique_fd fileitem::GetFileFd() {
	LOGSTART("fileitem::GetFileFd");
	setLockGuard;

	ldbg("Opening " << m_sPathRel);
	int fd=open(SZABSPATH(m_sPathRel), O_RDONLY);

#ifdef HAVE_FADVISE
	// optional, experimental
	if(fd != -1)
		posix_fadvise(fd, 0, m_nSizeChecked, POSIX_FADV_SEQUENTIAL);
#endif

	return unique_fd(fd);
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

// XXX: bForce is ultima ratio and there should be a better way; draft in the revamp branch...
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

void fileitem::SetupComplete()
{
	setLockGuard;
	notifyAll();
	m_nSizeChecked = m_nSizeSeen;
	m_status = FIST_COMPLETE;
}

void fileitem::UpdateHeadTimestamp()
{
	if(m_sPathRel.empty())
		return;
	utimes(SZABSPATH(m_sPathRel + ".head"), nullptr);
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

inline void _LogWithErrno(const char *msg, const string & sFile)
{
	tErrnoFmter f;
	log::err(tSS() << sFile <<
			" storage error [" << msg << "], last errno: " << f);
}

#ifndef MINIBUILD

bool fileitem_with_storage::DownloadStartedStoreHeader(const header & h, size_t hDataLen,
		const char *pNextData,
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

	USRDBG( "Download started, storeHeader for " << m_sPathRel << ", current status: " << (int) m_status);
	
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

	m_nIncommingCount+=hDataLen;

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
			Truncate2checkedSize();
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
		{
			Truncate2checkedSize();
			checkforceclose(m_filefd);
		}

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
						if(cfg::debug & log::LOG_DEBUG)
							log::err(tSS() << "known data hit, don't write to: "<< m_sPathRel);
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

		m_bAllowStoreData = false;
		// have a clean header with just the error message
		m_head.frontLine = h.frontLine;
		m_head.set(header::CONTENT_LENGTH, "0");
		if(m_status > FIST_DLGOTHEAD)
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

		m_filefd = open(sPathAbs.c_str(), flags, cfg::fileperms);
		ldbg("file opened?! returned: " << m_filefd);

		// self-recovery from cache poisoned with files with wrong permissions
		if (m_filefd < 0)
		{
			if(m_nSizeChecked > 0) // OOOH CRAP! CANNOT APPEND HERE! Do what's still possible.
			{
				string temp = sPathAbs + ".tmp";
				if(FileCopy(sPathAbs, temp) && 0 == unlink(sPathAbs.c_str()) )
				{
					if(0!=rename(temp.c_str(), sPathAbs.c_str()))
						return withError("503 Cannot rename files");

					// be sure about that
					if(0!=stat(sPathAbs.c_str(), &stbuf) || stbuf.st_size!=m_nSizeSeen)
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

		if(0 != fstat(m_filefd, &stbuf) || !S_ISREG(stbuf.st_mode))
			return withErrorAndKillFile("503 Not a regular file");

		// this makes sure not to truncate file while it's mmaped
		auto tempLock = TFileShrinkGuard::Acquire(stbuf);

		// crop, but only if the new size is smaller. MUST NEVER become larger (would fill with zeros)
		if(m_nSizeSeen != 0 && m_nSizeChecked <= m_nSizeSeen)
		{
			if(0==Truncate2checkedSize())
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

		//	falloc_helper(m_filefd, hint_start, hint_length);
		if(m_nSizeSeen < (off_t) cfg::allocspace)
			falloc_helper(m_filefd, 0, min(hint_start+hint_length, (off_t)cfg::allocspace));
		/*
		 * double-check the docs, this is probably not relevant for writting
#ifdef HAVE_FADVISE
	// optional, experimental
		posix_fadvise(m_filefd, hint_start, hint_length, POSIX_FADV_SEQUENTIAL);
#endif
		 */
		ldbg("Storing header as " + sHeadPath);
		int count=m_head.StoreToFile(sHeadPath);

		if(count<0)
			return withErrorAndKillFile( (-count != ENOSPC
					? "503 Cache storage error" : "503 OUT OF DISK SPACE"));

		// double-check the sane state
		if(0 != fstat(m_filefd, &stbuf) || stbuf.st_size != m_nSizeChecked)
			return withErrorAndKillFile("503 Inconsistent file state");

		if(m_nSizeChecked != lseek(m_filefd, m_nSizeChecked, SEEK_SET))
			return withErrorAndKillFile("503 IO error, positioning");
	}

	m_status=FIST_DLGOTHEAD;
	return true;
}

bool fileitem_with_storage::StoreFileData(const char *data, unsigned int size)
{
	setLockGuard;

	LOGSTART2("fileitem::StoreFileData", "status: " <<  (int) m_status << ", size: " << size);

	// something might care, most likely... also about BOUNCE action
	notifyAll();

	m_nIncommingCount += size;

	if(m_status > FIST_COMPLETE || m_status < FIST_DLGOTHEAD)
	{
		ldbg("StoreFileData rejected, status: " << (int) m_status)
		return false;
	}

	if (size == 0)
	{
		if(FIST_COMPLETE == m_status)
		{
			LOG("already completed");
		}
		else
		{
			m_status = FIST_COMPLETE;
			m_nTimeDlDone = GetTime();

			if (cfg::debug & log::LOG_MORE)
				log::misc(tSS() << "Download of " << m_sPathRel << " finished");

			// we are done! Fix header from chunked transfers?
			if (m_filefd >= 0 && !m_head.h[header::CONTENT_LENGTH])
			{
				m_head.set(header::CONTENT_LENGTH, m_nSizeChecked);
				// only update the file on disk if this item is still shared
				lockguard lockGlobalMap(mapItemsMx);
				if(m_globRef != mapItems.end())
					m_head.StoreToFile(SABSPATHEX(m_sPathRel, ".head"));
			}
		}
	}
	else
	{
		m_status = FIST_DLRECEIVING;

		if (m_bAllowStoreData && m_filefd >= 0)
		{
			while(size > 0)
			{
				int r = write(m_filefd, data, size);
				if(r < 0)
				{
					if(EINTR == errno || EAGAIN == errno)
						continue;
					tErrnoFmter efmt("HTTP/1.1 503 ");
					m_head.frontLine = efmt;
					m_status=FIST_DLERROR;
					// message will be set by the caller
					m_nTimeDlDone = GetTime();
					_LogWithErrno(efmt.c_str(), m_sPathRel);
					return false;
				}
				m_nSizeChecked += r;
				size -= r;
				data += r;
			}
		}
	}

	// needs to remember the good size, just in case the DL is resumed (restarted in hot state)
	if(m_nSizeSeen < m_nSizeChecked)
		m_nSizeSeen = m_nSizeChecked;

	return true;
}

TFileItemUser::~TFileItemUser()
{
	LOGSTART("TFileItemUser::~TFileItemUser");

	lockguard managementLock(mapItemsMx);

	if (!m_ptr) // unregistered before? or not shared?
		return;

	auto ucount = --m_ptr->usercount;
	if (ucount > 0)
		return; // still used

	// invalid or not globally registered?
	if (m_ptr->m_globRef == mapItems.end())
		return;

	// some file items will be held ready for some time

	if (MAXTEMPDELAY && m_ptr->m_bCheckFreshness
			&& m_ptr->m_status == fileitem::FIST_COMPLETE)
	{
		auto when = m_ptr->m_nTimeDlStarted + MAXTEMPDELAY;
		if (when > GetTime())
		{
			cleaner::GetInstance().ScheduleFor(when, cleaner::TYPE_EXFILEITEM);
			return;
		}
	}
	// no more readers for this item, can be discarded
	mapItems.erase(m_ptr->m_globRef);
}

TFileItemUser TFileItemUser::Create(cmstring &sPathUnescaped, bool makeWay)
{
	LOGSTART2s("TFileItemUser::Create", sPathUnescaped);
	TFileItemUser ret;

	try
	{
		mstring sPathRel(fileitem_with_storage::NormalizePath(sPathUnescaped));
		lockguard lockGlobalMap(mapItemsMx);
		auto it = mapItems.find(sPathRel);
		if(it != mapItems.end())
		{
			auto& fi = it->second;
			if (makeWay && !fi->m_sPathRel.empty())
			{
				// detect items that got stuck somehow and move it out of the way
				time_t now(GetTime());
				if(fi->m_nTimeDlDone > now + MAXTEMPDELAY  + 2
						|| fi->m_nTimeDlStarted > now - cfg::stucksecs)
				{

					auto pathAbs = SABSPATH(fi->m_sPathRel);
					auto xName = pathAbs + ltos(now);
					if(0 != creat(xName.c_str(), cfg::fileperms))
					{
						// oh, that's bad, no permissions on the folder whatsoever?
						log::err(string("Failure to create replacement of ") + fi->m_sPathRel + " - CHECK FOLDER PERMISSIONS!");
					}
					else
					{
						// if we can create files there then renaming should not be a problem
						unlink(pathAbs.c_str());
						rename(xName.c_str(), pathAbs.c_str());
						fi->m_globRef = mapItems.end();
						mapItems.erase(it);
						goto add_as_new;
					}
				}
			}
			LOG("Sharing existing file item");
			it->second->usercount++;
			ret.m_ptr = it->second;
			return ret;
		}
		else
		{
			add_as_new:
			LOG("Registering the NEW file item...");
			auto sp(make_shared<fileitem_with_storage>(sPathRel));
			sp->usercount++;
			auto res = mapItems.emplace(sPathRel, sp);
			ASSERT(res.second);
			sp->m_globRef = res.first;
			//lockGlobalMap.unLock();
			ret.m_ptr = sp;
			return ret;
		}
	}
	catch(std::bad_alloc&)
	{
		return TFileItemUser();
	}
}

// make the fileitem globally accessible
TFileItemUser TFileItemUser::Create(tFileItemPtr spCustomFileItem, bool isShareable)
{
	LOGSTART2s("TFileItemUser::Create", spCustomFileItem->m_sPathRel);

	TFileItemUser ret;

	if (!spCustomFileItem || spCustomFileItem->m_sPathRel.empty())
		return ret;


	if(!isShareable)
	{
		ret.m_ptr = spCustomFileItem;
		return ret;
	}

	lockguard lockGlobalMap(mapItemsMx);

	auto installed = mapItems.emplace(spCustomFileItem->m_sPathRel,
			spCustomFileItem);

	if(!installed.second)
		return ret; // conflict, another agent is already active

	spCustomFileItem->m_globRef = installed.first;
	spCustomFileItem->usercount++;
	ret.m_ptr = spCustomFileItem;
	return ret;
}

// this method is supposed to be awaken periodically and detects items with ref count manipulated by
// the request storm prevention mechanism. Items shall be be dropped after some time if no other
// thread but us is using them.
time_t TFileItemUser::BackgroundCleanup()
{
	LOGSTART2s("fileItemMgmt::BackgroundCleanup", GetTime());
	lockguard lockGlobalMap(mapItemsMx);

	time_t now = GetTime();
	time_t oldestGet = END_OF_TIME;
	time_t expBefore = now - MAXTEMPDELAY;

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

	// preserving a few seconds to catch more of them in the subsequent run
	return std::max(oldestGet + MAXTEMPDELAY, GetTime() + 8);
}

ssize_t fileitem_with_storage::SendData(int out_fd, int in_fd, off_t &nSendPos, size_t count)
{
	if(out_fd == -1 || in_fd == -1)
		return -1;

#ifndef HAVE_LINUX_SENDFILE
	return sendfile_generic(out_fd, in_fd, &nSendPos, count);
#else
	ssize_t r=sendfile(out_fd, in_fd, &nSendPos, count);

	if(r<0 && (errno == ENOSYS || errno == EINVAL))
		return sendfile_generic(out_fd, in_fd, &nSendPos, count);

	return r;
#endif
}

void TFileItemUser::dump_status()
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
					<< item.second->m_nSizeChecked << " , "
					<< item.second->m_nSizeSeen
					<< "\n\tGotAt: " << item.second->m_nTimeDlStarted << "\n\n";
		}
		log::err(fmt);
	}
	log::flush();
}

fileitem_with_storage::~fileitem_with_storage()
{
	Truncate2checkedSize();
}

int fileitem_with_storage::Truncate2checkedSize()
{
	if(m_filefd == -1 || m_nSizeChecked<0)
		return -1;
	return ftruncate(m_filefd, m_nSizeChecked);
}

// special file? When it's rewritten from start, save the old version instead
int fileitem_with_storage::MoveRelease2Sidestore()
{
	if(m_nSizeChecked)
		return 0;
	if(!endsWithSzAr(m_sPathRel, "/InRelease") && !endsWithSzAr(m_sPathRel, "/Release"))
		return 0;
	auto srcAbs = CACHE_BASE + m_sPathRel;
	Cstat st(srcAbs);
	if(st)
	{
		auto tgtDir = CACHE_BASE + cfg::privStoreRelSnapSufix + sPathSep + GetDirPart(m_sPathRel);
		mkdirhier(tgtDir);
		auto sideFileAbs = tgtDir + ltos(st.st_ino) + ltos(st.st_mtim.tv_sec)
				+ ltos(st.st_mtim.tv_nsec);
		return FileCopy(srcAbs, sideFileAbs);
		//return rename(srcAbs.c_str(), sideFileAbs.c_str());
	}
	return 0;
}

#endif // MINIBUILD

}
