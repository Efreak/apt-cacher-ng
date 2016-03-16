//============================================================================
// Name        : acngfs.cpp
// Author      : Eduard Bloch
// Description : Simple FUSE-based filesystem for HTTP access (apt-cacher NG)
//============================================================================


#define LOCAL_DEBUG
#include "debug.h"

#include "acsyscap.h"

#include "meta.h"
#include "header.h"
#include "caddrinfo.h"
#include "sockio.h"
#include "acbuf.h"
#include "acfg.h"
#include "lockable.h"
#include "cleaner.h"
#include "tcpconnect.h"

#include "fileitem.h"
#include "dlcon.h"

#include <sys/types.h>
#include <sys/stat.h>
#ifdef HAVE_SYS_MOUNT_H
#include <sys/param.h>
#include <sys/mount.h>
#endif
#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif

#include <unistd.h>
#include <inttypes.h>
#include <stdint.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>

#include <cstdio>
#include <algorithm>
#include <iostream>
#include <list>


#define FUSE_USE_VERSION 25
#include <fuse.h>

#ifdef HAVE_DLOPEN
#include <dlfcn.h>
#endif

#define HEADSZ 5000
#ifndef MIN
#define MIN(a,b) ( (a<=b)?a:b)
#endif

using namespace std;

const string sEmptyString;

#ifdef SPAM
#define _cerr(x) cerr << x
#warning printing spam all around
#else
#define _cerr(x)
#endif

#define POOLMAXSIZE 20 // max size
#define POOLMAXAGE 50 // seconds

// some globals, set only once
static struct stat statTempl;
static struct statfs stfsTemp;
static tHttpUrl baseUrl, proxyUrl;
static mstring altPath;
bool g_bGoodServer=true;

cmstring sDefPortHTTP("3142"), sDefPortHTTPS("80");

struct tDlDesc
{
	cmstring m_path;
	uint m_ftype;

	virtual int Read(char *retbuf, const char *path, off_t pos, size_t len) =0;
	virtual int Stat(struct stat &stbuf) =0;
	tDlDesc(cmstring &p, uint ftype) : m_path(p), m_ftype(ftype) {};
	virtual ~tDlDesc() {};
};

struct tDlDescLocal : public tDlDesc
{
	FILE *pFile;
	tDlDescLocal(cmstring &path, uint ftype) : tDlDesc(path, ftype), pFile(nullptr)
	{
	};

	int Stat(struct stat &stbuf)
	{
		if(altPath.empty()) // hm?
			return -ENOENT;

		if (::stat((altPath + m_path).c_str(), &stbuf))
			return -errno;

		// verify the file state
		header h;
		int r = h.LoadFromFile(altPath + m_path + ".head");
		if (r <= 0 || stbuf.st_size != atoofft(h.h[header::CONTENT_LENGTH], -23))
			return -EIO;

		return 0;
	}

	virtual ~tDlDescLocal()
	{
		if(pFile)
			fclose(pFile);
		pFile=nullptr;
	};

	int Read(char *retbuf, const char *path, off_t pos, size_t len)
	{
		if (!pFile)
		{
			struct stat stbuf;
			if(Stat(stbuf))
				return -EIO; // file incomplete or missing

			FILE *pf = fopen((altPath + m_path).c_str(), "rb");
			if (!pf)
				return -EIO;
			pFile = pf;
		}

		int copied=0;
		if(pFile && 0==fseeko(pFile, pos, SEEK_SET))
		{
			while(!feof(pFile) && !ferror(pFile) && len>0)
			{
				size_t r = ::fread(retbuf+copied, 1, len, pFile);
				copied+=r;
				len-=r;
			}
		}
		return ferror(pFile) ? -EIO : copied;
	}
};

struct tFileId
{ off_t m_size; mstring m_ctime;
tFileId() : m_size(0) {};
tFileId(off_t a, mstring b) : m_size(a), m_ctime(b) {};
bool operator!=(tFileId other) const { return m_size != other.m_size || m_ctime != other.m_ctime;}
};
static class : public lockable, public map<string, tFileId>
{} remote_info_cache;

struct tDlDescRemote : public tDlDesc
{
protected:

	tFileId fid;
	bool bIsFirst; // hint to catch the validation data when download starts

public:
	tDlDescRemote(cmstring &p, uint n) : tDlDesc(p,n), bIsFirst(true)
	{
		// expire the caches every time, should not cost much anyway
		g_tcp_con_factory.BackgroundCleanup();
		CAddrInfo::BackgroundCleanup();
	};

	int Read(char *retbuf, const char *path, off_t pos, size_t len)
	{
		dlcon dler(true, 0);
		tHttpUrl uri = proxyUrl;
		uri.sPath += baseUrl.sHost
		// + ":" + ( baseUrl.sPort.empty() ? baseUrl.sPort : "80")
				+ baseUrl.sPath + m_path;
		class tFitem: public fileitem
		{
		public:
			char *pRet;
			size_t nRest, nGot;
			off_t skipBytes;
			int nErr;

			ssize_t SendData(int, int, off_t&, size_t) override
			{
				return 0;
			} // nothing to send
			bool StoreFileData(const char *p, unsigned int count) override
			{
				if (count == 0)
				{
					m_status=FIST_COMPLETE;
					return true;
				}

				if(skipBytes>0)
				{
					if(skipBytes>count)
					{
						skipBytes-=count;
						return true;
					}
					count-=skipBytes;
					p+=skipBytes;
					skipBytes=0;
				}

				if(!nRest)
				{
					m_status=FIST_COMPLETE;
					return false;
				}
				if(count>nRest)
					count=nRest;
				memcpy(pRet+nGot, p, count);
				nGot+=count;
				nRest-=count;
				return true;
			}
#define SETERROR { nErr=__LINE__; return false;}
			bool &m_isFirst;
			bool DownloadStartedStoreHeader(const header &head, size_t, const char*, bool bRestarted, bool&) override
			{
				_cerr(head.frontLine<<endl);
				m_head = head; // XXX: bloat, only status line and contlen required
				int st =head.getStatus();

				if(st == 416)
					return true; // EOF

				if(bRestarted) // throw the head away, the data should be ok
					return true; // XXX, add more checks?

				if(st != 200 && st != 206)
				{
					SETERROR;
				}

				// validation
				if (head.h[header::LAST_MODIFIED])
				{
					if (m_isFirst)
						fid.m_ctime = head.h[header::LAST_MODIFIED];
					else if (fid.m_ctime != head.h[header::LAST_MODIFIED])
						SETERROR;
				}

				off_t myfrom(0), myto(0), mylen(0);
				const char *p=head.h[header::CONTENT_RANGE];
				if(p)
				{
					int n=sscanf(p, "bytes " OFF_T_FMT "-" OFF_T_FMT "/" OFF_T_FMT, &myfrom, &myto, &mylen);
					if(n<=0)
						n=sscanf(p, "bytes=" OFF_T_FMT "-" OFF_T_FMT "/" OFF_T_FMT, &myfrom, &myto, &mylen);
					if(n!=3  // check for nonsense
							|| (m_nSizeSeen>0 && myfrom != m_nSizeSeen-1)
							|| (m_nRangeLimit>=0 && myto > m_nRangeLimit) // too much data?
							|| myfrom<0 || mylen<0
					)
					{
						SETERROR;
					}

				}
				else if(st == 200 && head.h[header::CONTENT_LENGTH])
					mylen = atoofft(head.h[header::CONTENT_LENGTH]);

				// validation
				if(m_isFirst)
					fid.m_size = mylen;
				else
					if(fid.m_size != mylen)
						SETERROR;

				skipBytes -= myfrom;
				if(skipBytes<0)
					SETERROR;
				return true;
			}
			tFileId &fid;
			tFitem(char *p, size_t size, off_t start, tFileId &fi, bool &isfirst)
			: pRet(p), nRest(size),
					nGot(0), skipBytes(start), nErr(0), m_isFirst(isfirst), fid(fi)
			{
				m_bCheckFreshness = false;
				m_nSizeSeen = start;
				m_nRangeLimit = g_bGoodServer ? start+size-1 : -1;
			}
		};

		{
			lockguard g(remote_info_cache);
			map<string, tFileId>::const_iterator it = remote_info_cache.find(path);
			if (it != remote_info_cache.end())
				fid = it->second;
		}
		tFileId fidOrig=fid;

		tFitem *pFi = new tFitem(retbuf, len, pos, fid, bIsFirst);
		tFileItemPtr spFi(static_cast<fileitem*>(pFi));
		dler.AddJob(spFi, &uri, 0, 0, 0);
		dler.WorkLoop();
		int nHttpCode(100);
		pFi->WaitForFinish(&nHttpCode);
		bIsFirst=false;


		if (m_ftype == rechecks::FILE_SOLID && fidOrig != fid)
		{
			lockguard g(remote_info_cache);
			remote_info_cache[m_path] = fid;
		}

		if(nHttpCode==416)
			return 0; // EOF
		if(pFi->nErr || !pFi->nGot)
			return -EIO;
		return pFi->nGot;
	}

	int Stat(struct stat &stbuf)
	{
		stbuf = statTempl;
		{
			lockguard g(remote_info_cache);
			map<string, tFileId>::const_iterator it = remote_info_cache.find(m_path);
			if (it != remote_info_cache.end())
			{
				stbuf.st_size = it->second.m_size;
				stbuf.st_mode &= ~S_IFDIR;
				stbuf.st_mode |= S_IFREG;
				struct tm tmx;
				if(header::ParseDate(it->second.m_ctime.c_str(), &tmx))
					stbuf.st_ctime = mktime(&tmx);
				_cerr("Using precached\n");
				return 0;
			}
		}
		// ok, not cached, do the hard way

		dlcon dler(true, 0);

		tHttpUrl uri = proxyUrl;
		uri.sPath += baseUrl.sHost
		// + ":" + ( baseUrl.sPort.empty() ? baseUrl.sPort : "80")
				+ baseUrl.sPath + m_path;
		class tFitemProbe: public fileitem
		{
		public:
			ssize_t SendData(int, int, off_t&, size_t) override
			{
				return 0;
			} // nothing to send
			bool StoreFileData(const char*, unsigned int) override
			{
				return false;
			}
			tFitemProbe()
			{
				m_bHeadOnly = true;
			}
			bool DownloadStartedStoreHeader(const header &head, size_t, const char*,
					bool bRestart, bool&) override
			{
				if(bRestart)
					return true;

				m_head = head; // XXX: bloat, only status line and contlen required
				m_status = FIST_COMPLETE;
				return true;
			}
		};
		auto probe(make_shared<tFitemProbe>());
		dler.AddJob(probe, &uri, 0, 0, 0);
		dler.WorkLoop();
		int nHttpCode(100);
		fileitem::FiStatus res = probe->WaitForFinish(&nHttpCode);
		stbuf.st_size = atoofft(probe->GetHeaderUnlocked().h[header::CONTENT_LENGTH], 0);
		stbuf.st_mode &= ~S_IFDIR;
		stbuf.st_mode |= S_IFREG;

		if (res < fileitem::FIST_COMPLETE)
			return -EIO;
		else if (nHttpCode == 200)
		{
			if (m_ftype == rechecks::FILE_SOLID) // not caching volatile stuff
			{
				lockguard g(remote_info_cache);
				remote_info_cache[m_path] =
						tFileId(stbuf.st_size, probe->GetHeaderUnlocked().h[header::LAST_MODIFIED]);
			}
			struct tm tmx;
			if(header::ParseDate(probe->GetHeaderUnlocked().h[header::LAST_MODIFIED], &tmx))
				stbuf.st_ctime = mktime(&tmx);
			return 0;
		}
		return -ENOENT;
	}
};



/// If found as downloadable, present as a file, or become a directory otherwise.
static int acngfs_getattr(const char *path, struct stat *stbuf)
{
	if(!path)
		return -1;

	rechecks::eMatchType type = rechecks::GetFiletype(path);
	_cerr( "type: " << type);
	if (type == rechecks::FILE_SOLID || type == rechecks::FILE_VOLATILE)
	{
		if(0 == tDlDescLocal(path, type).Stat(*stbuf))
			return 0;
		if(0 == tDlDescRemote(path, type).Stat(*stbuf))
			return 0;
	}

	//ldbg("Be a directory!");
	memcpy(stbuf, &statTempl, sizeof(statTempl));
	stbuf->st_mode &= ~S_IFMT; // delete mode flags and set them as needed
	stbuf->st_mode |= S_IFDIR;
	stbuf->st_size = 4;
	return 0;
}

static int acngfs_fgetattr(const char *path, struct stat *stbuf,
      struct fuse_file_info *fi)
{
	// FIXME: reuse the con later? or better not, size might change during operation
	return acngfs_getattr(path, stbuf);
}

static int acngfs_access(const char *path, int mask)
{
	// non-zero (failure) when trying to write
   return mask&W_OK;
}

static int acngfs_readlink(const char *path, char *buf, size_t size)
{
   return -EINVAL;
}

static int acngfs_opendir(const char *path, struct fuse_file_info *fi)
{
	// let FUSE manage directories
	return 0;
}

static int acngfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
      off_t offset, struct fuse_file_info *fi)
{   
	return -EPERM;
}

static int acngfs_releasedir(const char *path, struct fuse_file_info *fi)
{

   return 0;
}

static int acngfs_mknod(const char *path, mode_t mode, dev_t rdev)
{
	return -EROFS;
}

static int acngfs_mkdir(const char *path, mode_t mode)
{
   return -EROFS;
}

static int acngfs_unlink(const char *path)
{
   return -EROFS;
}

static int acngfs_rmdir(const char *path)
{
   return -EROFS;
}

static int acngfs_symlink(const char *from, const char *to)
{
  return -EROFS;
}

static int acngfs_rename(const char *from, const char *to)
{
  return -EROFS;
}

static int acngfs_link(const char *from, const char *to)
{
	return -EROFS;
}

static int acngfs_chmod(const char *path, mode_t mode)
{
   return -EROFS;
}

static int acngfs_chown(const char *path, uid_t uid, gid_t gid)
{
return -EROFS;
}

static int acngfs_truncate(const char *path, off_t size)
{
   return -EROFS;
}

static int acngfs_ftruncate(const char *path, off_t size,
      struct fuse_file_info *fi)
{
return -EROFS;
}

static int acngfs_utime(const char *path, struct utimbuf *buf)
{
 return -EROFS;
}

//lockable mxTest;

static int acngfs_open(const char *path, struct fuse_file_info *fi)
{
	//lockguard g(&mxTest);
	
	if (fi->flags & (O_WRONLY|O_RDWR|O_TRUNC|O_CREAT))
			return -EROFS;

	tDlDesc *p(nullptr);
	struct stat stbuf;
	rechecks::eMatchType ftype = rechecks::GetFiletype(path);

	MYTRY
	{
		// ok... if that is a remote object, can we still use local access instead?
		if(!altPath.empty() && rechecks::FILE_SOLID == ftype)
		{
				p = new tDlDescLocal(path, ftype);
				if(p)
				{
					if(0==p->Stat(stbuf))
						goto desc_opened;
					delete p;
					p=nullptr;
				}
		}


		p=new tDlDescRemote(path, ftype);
		if(!p) // running exception-free? 
			return -EIO;
		if(0!=p->Stat(stbuf))
		{
			delete p;
			return -EIO;
		}
	}
	MYCATCH(std::bad_alloc&)
	{
		return -EIO;
	}
	
	desc_opened:

	fi->fh = (uintptr_t) p;
	return 0;
}


static int acngfs_read(const char *path, char *buf, size_t size, off_t offset,
      struct fuse_file_info *fi)
{
	auto p=(tDlDesc*) fi->fh;
	return p->Read(buf, path, offset, size);
}

static int acngfs_write(const char *path, const char *buf, size_t size,
      off_t offset, struct fuse_file_info *fi)
{
	return -EBADF;
}

static int acngfs_statfs(const char *path, struct statvfs *stbuf)
{
   memcpy(stbuf, &stfsTemp, sizeof(*stbuf));
	return 0;
}

static int acngfs_release(const char *path, struct fuse_file_info *fi)
{
	if(fi->fh)
		delete (tDlDesc*)fi->fh;
	return 0;
}

static int acngfs_fsync(const char *path, int isdatasync,
      struct fuse_file_info *fi)
{
   return 0;
}


struct fuse_operations_compat25 acngfs_oper;

int my_fuse_main(int argc, char ** argv)
{
#ifdef HAVE_DLOPEN
   auto pLib = dlopen("libfuse.so.2", RTLD_LAZY);
   if(!pLib)
   {
      cerr << "Couldn't find libfuse.so.2" <<endl;
      return -1;
   }
   auto myFuseMain = (decltype(&fuse_main_real_compat25)) dlsym(pLib, "fuse_main_real_compat25");
   if(!myFuseMain)
   {
	  cerr << "Error loading libfuse.so.2" <<endl;
	  return -2;
   }
   return (*myFuseMain) (argc, argv, &acngfs_oper, sizeof(acngfs_oper));
#else
#warning dlopen not available
   return fuse_main(argc, argv, &acngfs_oper);
#endif
}

void _ExitUsage() {
   cerr << "USAGE: acngfs BaseURL ProxyHost MountPoint [FUSE Mount Options]\n"
   << "examples:\n\t  acngfs http://ftp.uni-kl.de/debian cacheServer:3142 /var/local/aptfs\n"
   << "\t  acngfs http://ftp.uni-kl.de/debian localhost:3142 /var/cache/apt-cacher-ng/debrep /var/local/aptfs\n\n"
        << "FUSE mount options summary:\n\n";
    char *argv[] = {strdup("..."), strdup("-h")};
    my_fuse_main( _countof(argv), argv);
    exit(EXIT_FAILURE);
}

#define barf(x) { cerr << endl << "ERROR: " << x <<endl; exit(1); }
#define erUsage { _ExitUsage(); }

int main(int argc, char *argv[])
{
   memset(&acngfs_oper, 0, sizeof(acngfs_oper));

   acngfs_oper.getattr	= acngfs_getattr;
   acngfs_oper.fgetattr	= acngfs_fgetattr;
   acngfs_oper.access	= acngfs_access;
   acngfs_oper.readlink	= acngfs_readlink;
   acngfs_oper.opendir	= acngfs_opendir;
   acngfs_oper.readdir	= acngfs_readdir;
   acngfs_oper.releasedir	= acngfs_releasedir;
   acngfs_oper.mknod	= acngfs_mknod;
   acngfs_oper.mkdir	= acngfs_mkdir;
   acngfs_oper.symlink	= acngfs_symlink;
   acngfs_oper.unlink	= acngfs_unlink;
   acngfs_oper.rmdir	= acngfs_rmdir;
   acngfs_oper.rename	= acngfs_rename;
   acngfs_oper.link	= acngfs_link;
   acngfs_oper.chmod	= acngfs_chmod;
   acngfs_oper.chown	= acngfs_chown;
   acngfs_oper.truncate	= acngfs_truncate;
   acngfs_oper.ftruncate	= acngfs_ftruncate;
   acngfs_oper.utime	= acngfs_utime;
//   acngfs_oper.create	= acngfs_create;
   acngfs_oper.open	= acngfs_open;
   acngfs_oper.read	= acngfs_read;
   acngfs_oper.write	= acngfs_write;
   acngfs_oper.statfs	= acngfs_statfs;
   acngfs_oper.release	= acngfs_release;
   acngfs_oper.fsync	= acngfs_fsync;

   umask(0);

   for(int i = 1; i<argc; i++)
   	   if(argv[i] && 0==strcmp(argv[i], "--help"))
   		   erUsage;
   
   if(argc<4)
      barf("Not enough arguments, try --help.\n");
   
   acfg::agentname = "ACNGFS";
   acfg::agentheader="User-Agent: ACNGFS\r\n";
   acfg::requestapx = "User-Agent: ACNGFS\r\nX-Original-Source: 42\r\n";
#ifdef SPAM
   acfg::debug=0xff;
   acfg::verboselog=1;
#endif

   if(argv[1] && baseUrl.SetHttpUrl(argv[1]))
   {
#ifdef VERBOSE
	   cout << "Base URL: " << baseUrl.ToString()<<endl;
#endif
   }
   else
   {
      cerr << "Invalid base URL, " << argv[1] <<endl;
      exit(EXIT_FAILURE);
   }
   // FUSE adds starting / already, drop ours if present
   trimBack(baseUrl.sPath, "/");
   
   if(argv[2] && proxyUrl.SetHttpUrl(argv[2]))
   {
	   /*if(proxyUrl.GetPort().empty())
		   proxyUrl.sPort="3142";
		   */
   }
   else
   {
	   cerr << "Invalid proxy URL, " << argv[2] <<endl;
	   exit(EXIT_FAILURE);
   }

   // all parameters processed, forwarded to fuse call below

   ::rechecks::CompileExpressions();
   
#if 0//def SPAM
   {
	   fuse_file_info fi = {0};
	   const char *dingsda="/dists/unstable/InRelease";
	   acngfs_open(dingsda, &fi);
	   char buf[165536];
	   off_t pos=0;
	   for(;0 < acngfs_read(dingsda, buf, sizeof(buf), pos, &fi); pos+=sizeof(buf)) ;
	   return 0;
   }
#endif

   unsigned int nMyArgCount = 2; // base url, proxy host
   // alternative path supplied in the next argument?
   if(argc > 4 && argv[4] && argv[4][0] != '-' ) // 4th argument is not an option?
   {
	   nMyArgCount=3;
	   altPath = argv[3];
   }


   
   // test mount point
   const char *mpoint = argv[nMyArgCount+1];
   if(stat(mpoint, &statTempl) || statfs(mpoint, &stfsTemp))
	   barf(endl << "Cannot access " << mpoint);
   if(!S_ISDIR(statTempl.st_mode))
      barf(endl<< mpoint << " is not a directory.");

   // skip our arguments, keep those for fuse including mount point and argv[0] at the right place
   argv[nMyArgCount]=argv[0]; // application path
   argv=&argv[nMyArgCount];
   argc-=nMyArgCount;
   return my_fuse_main(argc, argv);
}

#ifndef DEBUG
// for the uber-clever GNU linker and should be removed by strip again
namespace aclog
{
   void flush() {};
   void misc(const string &s, const char )
   {
#ifdef SPAM
      cerr << s << endl;
#endif
   }

void err(const char *s, const char* z)
{
#ifdef SPAM
      cerr << s << endl << z << endl;
#endif

};
}
#endif
