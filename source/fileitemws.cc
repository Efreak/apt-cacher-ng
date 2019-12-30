#include "fileitem.h"
#include "fileio.h"
#include "acfg.h"
#include "debug.h"
#include "filelocks.h"

using namespace std;

namespace acng
{

extern tFiGlobMap mapItems;
extern acmutex mapItemsMx;


// dl item implementation with storage on disk
class fileitem_with_storage : public fileitem
{
	unique_fd m_filefd;

public:
	inline fileitem_with_storage(cmstring &s) {m_sCachePathRel=s;};
	virtual ~fileitem_with_storage();
	int Truncate2checkedSize() override;

	// Interface implementation: data sink
	ssize_t SendData(int confd, int filefd, off_t &nSendPos, size_t nMax2SendNow) override;
	bool StoreBody(const char *data, unsigned int size) override;


	unique_fd GetFileFd() override
	{
		LOGSTART("fileitem::GetFileFd");
		setLockGuard;

		ldbg("Opening " << m_sCachePathRel);
		int fd = open(SZABSPATH(m_sCachePathRel), O_RDONLY);

	#ifdef HAVE_FADVISE
		// optional, experimental
		if(fd != -1)
			posix_fadvise(fd, 0, m_nSizeChecked, POSIX_FADV_SEQUENTIAL);
	#endif

		return unique_fd(fd);
	}


	void UpdateUseDate() override
	{
		if(m_sCachePathRel.empty())
			return;
		::utimes(SZABSPATHEX(m_sCachePathRel, ".head"), nullptr);
	}

protected:
	int MoveRelease2Sidestore();

	void SinkDestroy(bool delHed, bool delData) override
	{
		auto path = SABSPATH(m_sCachePathRel);
		if(delData)
			::unlink(path.c_str());
		if(delHed)
			::unlink((path+".head").c_str());
	}

	void DetectNotModifiedDownload(char nextByte, off_t mylen) override
	{
		if (m_nSizeSeen == mylen && m_nSizeChecked == mylen - 1)
		{
			int fd = open(SZABSPATH(m_sCachePathRel), O_RDONLY);
			if (fd != -1)
			{
				if (m_nSizeChecked == lseek(fd, m_nSizeChecked, SEEK_SET))
				{
					char c;
					if (1 == read(fd, &c, 1) && c == nextByte)
					{
						if (cfg::debug & log::LOG_DEBUG)
							log::err(
									tSS() << "known data hit, don't write to: "
											<< m_sCachePathRel);
						m_bAllowStoreData = false;
						m_nSizeChecked = mylen;
					}
				}
				forceclose(fd);
			}
		}
	}

	const char* SinkOpen(off_t hint_start, off_t hint_length) override
	{
		LOGSTART(__PRETTY_FUNCTION__);

		// using adaptive Delete-Or-Replace-Or-CopyOnWrite strategy

		MoveRelease2Sidestore();

		auto sPathAbs(SABSPATH(m_sCachePathRel));
		auto withErrorAndKillFile= [&](const char *msg)
		{
			SinkDestroy(true, true);
			return msg;
		};

		// First opening the file to be sure that it can be written. Header storage is the critical point,
		// every error after that leads to full cleanup to not risk inconsistent file contents

		int flags = O_WRONLY | O_CREAT | O_BINARY;
		struct stat stbuf;

		mkbasedir(sPathAbs);

		m_filefd = open(sPathAbs.c_str(), flags, cfg::fileperms);
		ldbg("file opened?! returned: " << *m_filefd);

		// self-recovery from cache poisoned with files with wrong permissions
		if (m_filefd == -1)
		{
			if(m_nSizeChecked > 0) // OOOH CRAP! CANNOT APPEND HERE! Do what's still possible.
			{
				string temp = sPathAbs + ".tmp";
				if(FileCopy(sPathAbs, temp) && 0 == unlink(sPathAbs.c_str()) )
				{
					if(0!=rename(temp.c_str(), sPathAbs.c_str()))
						return "503 Cannot rename files";

					// be sure about that
					if(0!=stat(sPathAbs.c_str(), &stbuf) || stbuf.st_size!=m_nSizeSeen)
						return "503 Cannot copy file parts, filesystem full?";

					m_filefd=open(sPathAbs.c_str(), flags, cfg::fileperms);
					ldbg("file opened after copying around: ");
				}
				else
				{
					log::err(tSS()<< "Error while creating files in " << GetDirPart(sPathAbs) << ": " << tErrnoFmter());
					return "503 Cannot store or remove files, check apt-cacher.err log";
				}
			}
			else
			{
				unlink(sPathAbs.c_str());
				m_filefd=open(sPathAbs.c_str(), flags, cfg::fileperms);
				ldbg("file force-opened?! returned: " << m_filefd);
			}
		}

		if (m_filefd == -1)
		{
			log::err(string(tErrnoFmter("Cache storage error - "))+sPathAbs);
			return "910 ACNG cache entry storage error";
		}

		if(0 != fstat(*m_filefd, &stbuf) || !S_ISREG(stbuf.st_mode))
			return withErrorAndKillFile("503 Not a regular file");

		// this makes sure not to truncate file while it's mmaped
		auto tempLock = TFileShrinkGuard::Acquire(stbuf);

		// crop, but only if the new size is smaller. MUST NEVER become larger (would fill with zeros)
		if(m_nSizeSeen != 0 && m_nSizeChecked <= m_nSizeSeen)
		{
			if(!SinkCtrl(true, true, true, false))
				return withErrorAndKillFile("503 Cannot change file size");
		}
		else if(m_nSizeChecked>m_nSizeSeen) // should never happen and caught by the checks above
			return withErrorAndKillFile("503 Internal error on size checking");
		// else... nothing to fix since the expectation==reality

		//	falloc_helper(m_filefd, hint_start, hint_length);
		if(m_nSizeSeen < (off_t) cfg::allocspace)
			falloc_helper(*m_filefd, 0, std::min(hint_start+hint_length, (off_t)cfg::allocspace));
		/*
		 * double-check the docs, this is probably not relevant for writting
#ifdef HAVE_FADVISE
	// optional, experimental
		posix_fadvise(m_filefd, hint_start, hint_length, POSIX_FADV_SEQUENTIAL);
#endif
		 */
		ldbg("Storing header as " + sPathAbs + ".head");
		int count=m_head.StoreToFile(sPathAbs + ".head");

		if(count<0)
			return withErrorAndKillFile( (-count != ENOSPC
					? "503 Cache storage error" : "503 OUT OF DISK SPACE"));

		// double-check the sane state
		if(0 != ::fstat(*m_filefd, &stbuf) || stbuf.st_size != m_nSizeChecked)
			return withErrorAndKillFile("503 Inconsistent file state");

//		if(m_nSizeChecked != lseek(m_filefd, m_nSizeChecked, SEEK_SET))
//			return withErrorAndKillFile("503 IO error, positioning");

		return nullptr;
	}

	bool SinkCtrl(bool errorAbort, bool sync, bool trim, bool close) override
	{
		bool ret = true;
		if(trim && m_filefd)
		{
			if(m_nSizeChecked<0)
				ret = false;
			else
				ret = 0 == ftruncate(*m_filefd, m_nSizeChecked);
		}
		if (errorAbort && !ret)
			return ret;

#if ( (_POSIX_C_SOURCE >= 199309L || _XOPEN_SOURCE >= 500) && _POSIX_SYNCHRONIZED_IO > 0)
		if (sync && m_filefd)
		{
			ret = (0 == fdatasync(*m_filefd) && errorAbort);
		}
		if(errorAbort && !ret)
			return ret;
#endif

		if(close)
			m_filefd.reset();

		return ret;
	}

	bool SinkRetire() override
	{
#warning retire flag setzen

		if (makeWay && !fi->m_sCachePathRel.empty())
				{
					// detect items that got stuck somehow and move it out of the way
					time_t now(GetTime());
					if(fi->m_nTimeDlDone > now + MAXTEMPDELAY  + 2
							|| fi->m_nTimeDlStarted > now - cfg::stucksecs)
					{

						auto pathAbs = SABSPATH(fi->m_sCachePathRel);
						auto xName = pathAbs + ltos(now);
						if(0 != creat(xName.c_str(), cfg::fileperms))
						{
							// oh, that's bad, no permissions on the folder whatsoever?
							log::err(string("Failure to create replacement of ") + fi->m_sCachePathRel + " - CHECK FOLDER PERMISSIONS!");
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
	}
};


fileitem_with_storage::~fileitem_with_storage()
{
	if(m_nSizeChecked >= 0)
		SinkCtrl(false, true, true, false);
	if(m_bIsRetired)
		SinkDestroy(true, true);
}

// special file? When it's rewritten from start, save the old version instead
int fileitem_with_storage::MoveRelease2Sidestore()
{
	if(m_nSizeChecked)
		return 0;
	if(!endsWithSzAr(m_sCachePathRel, "/InRelease") && !endsWithSzAr(m_sCachePathRel, "/Release"))
		return 0;
	auto srcAbs = CACHE_BASE + m_sCachePathRel;
	Cstat st(srcAbs);
	if(st)
	{
		auto tgtDir = CACHE_BASE + cfg::privStoreRelSnapSufix + sPathSep + GetDirPart(m_sCachePathRel);
		mkdirhier(tgtDir);
		auto sideFileAbs = tgtDir + ltos(st.st_ino) + ltos(st.st_mtim.tv_sec)
				+ ltos(st.st_mtim.tv_nsec);
		return FileCopy(srcAbs, sideFileAbs);
		//return rename(srcAbs.c_str(), sideFileAbs.c_str());
	}
	return 0;
}


bool fileitem_with_storage::StoreBody(const char *data, unsigned int size)
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
				log::misc(tSS() << "Download of " << m_sCachePathRel << " finished");

			// we are done! Fix header from chunked transfers?
			if (m_filefd >= 0 && !m_head.h[header::CONTENT_LENGTH])
			{
				m_head.set(header::CONTENT_LENGTH, m_nSizeChecked);
				// only update the file on disk if this item is still shared
				lockguard lockGlobalMap(mapItemsMx);
				if(m_globRef != mapItems.end())
					m_head.StoreToFile(SABSPATHEX(m_sCachePathRel, ".head"));
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
				int r = write(*m_filefd, data, size);
				if(r < 0)
				{
					if(EINTR == errno || EAGAIN == errno)
						continue;
					tErrnoFmter efmt("HTTP/1.1 503 ");
					m_head.frontLine = efmt;
					m_status=FIST_DLERROR;
					// message will be set by the caller
					m_nTimeDlDone = GetTime();
					_LogWithErrno(efmt.c_str(), m_sCachePathRel);
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



TFileItemUser fileitem::Create(cmstring& absPath, const struct stat& statInfo)
{
	/*
	 * This variant of file item handler sends a local file. The
	 * header data is generated as needed, the relative cache path variable
	 * is reused for the real path.
	 */
	class tLocalGetFitem : public fileitem_with_storage
	{
	public:
		tLocalGetFitem(string sLocalPath, const struct stat &stdata) :
			fileitem_with_storage(sLocalPath)
		{
			m_bAllowStoreData=false;
			m_status=FIST_COMPLETE;
			m_nSizeChecked=m_nSizeSeen=stdata.st_size;
			m_bCheckFreshness=false;
			m_head.type=header::ANSWER;
			m_head.frontLine="HTTP/1.1 200 OK";
			m_head.set(header::CONTENT_LENGTH, stdata.st_size);
			m_head.prep(header::LAST_MODIFIED, 26);
			if(m_head.h[header::LAST_MODIFIED])
				FormatTime(m_head.h[header::LAST_MODIFIED], 26, stdata.st_mtim.tv_sec);
			cmstring &sMimeType=cfg::GetMimeType(sLocalPath);
			if(!sMimeType.empty())
				m_head.set(header::CONTENT_TYPE, sMimeType);
		};
		unique_fd GetFileFd() override
		{
			int fd=open(m_sCachePathRel.c_str(), O_RDONLY);
		#ifdef HAVE_FADVISE
			// optional, experimental
			if(fd != -1)
				posix_fadvise(fd, 0, m_nSizeChecked, POSIX_FADV_SEQUENTIAL);
		#endif
			return unique_fd(fd);
		}
	};
	return TFileItemUser(std::make_shared<tLocalGetFitem>(absPath, statInfo));
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

TFileItemUser fileitem::Create(cmstring &sPathUnescaped, fileitem::ESharingStrategy onConflict)
{
	LOGSTART2s("fileitem::Create", sPathUnescaped);
	//LOG("Registering the NEW file item...");

	try
	{
		// :-( That might detox the path twice, but this seems to be the lesser evil
		return Install(move([&]() { return make_shared<fileitem_with_storage>(sPathUnescaped); }),
				DetoxPath4Cache(sPathUnescaped),
				onConflict);
	}
	catch(std::bad_alloc&)
	{
		return TFileItemUser();
	}
}

}
