
#include "fileio.h"
#include <unistd.h>
#include <dirent.h>
#include <string.h>
//#include "aclogger.h"

#include <set>

#include "meta.h"
#include "dirwalk.h"

using namespace std;
namespace acfg
{
extern int stupidfs;
}

struct dnode
{

	typedef pair<dev_t,ino_t> tPairDevIno;
	typedef set<tPairDevIno> tDupeFilter;
	
	dnode(dnode *parent) : m_parent(parent) {};
	bool Walk(IFileHandler *, tDupeFilter*, bool bFollowSymlinks);

	std::string sPath;
	dnode *m_parent;
	struct stat m_stinfo;


private:
	// not to be copied
	dnode& operator=(const dnode&);
	dnode(const dnode&);

};


bool dnode::Walk(IFileHandler *h, dnode::tDupeFilter *pFilter, bool bFollowSymlinks)
{
//	bool bNix=StrHas(sPath, "somehost");

	if(bFollowSymlinks)
	{
		if(stat(sPath.c_str(), &m_stinfo))
			return true; // slight risk of missing information here... bug ignoring is safer
	}
	else
	{
		auto r=lstat(sPath.c_str(), &m_stinfo);
		if(r)
		{
	/*		errnoFmter f;
				aclog::err(tSS() << sPath <<
						" IO error [" << f<<"]");
						*/
			return true; // slight risk of missing information here... bug ignoring is safer
		}
		// dangling symlink?
		if(S_ISLNK(m_stinfo.st_mode))
			return true;
	}
	
	if(S_ISREG(m_stinfo.st_mode)
#ifdef DEBUG
			|| S_ISBLK(m_stinfo.st_mode)
#endif
	)
		return h->ProcessRegular(sPath, m_stinfo);
	else if(! S_ISDIR(m_stinfo.st_mode))
		return h->ProcessOthers(sPath, m_stinfo);
	
	// ok, we are a directory, scan it and descend where needed

	// seen this in the path before? symlink cycle?
	for(dnode *cur=m_parent; cur!=nullptr; cur=cur->m_parent)
	{
		if (m_stinfo.st_dev == cur->m_stinfo.st_dev && m_stinfo.st_ino == cur->m_stinfo.st_ino)
			return true;
	}
	
	// also make sure we are not visiting the same directory through some symlink construct
	if(pFilter)
	{
#ifdef COMPATGCC47
               auto thisKey(make_pair(m_stinfo.st_dev, m_stinfo.st_ino));
               if(ContHas(*pFilter, thisKey))
                       return true;
               pFilter->insert(thisKey);
#else
		auto key_isnew = pFilter->emplace(m_stinfo.st_dev, m_stinfo.st_ino);
		if(!key_isnew.second)
			return true; // visited this before, recursion detected
#endif
	}

//	cerr << "Opening: " << sPath<<endl;
	DIR *dir = opendir(sPath.c_str());
	if (!dir) // weird, whatever... ignore...
		return true;
	
	struct dirent *dp;
	dnode childbuf(this);
	bool bRet(true);
	
	while ( nullptr != (dp = readdir(dir)) )
	{
		if (strcmp(dp->d_name, ".") && strcmp(dp->d_name, ".."))
		{
			childbuf.sPath=sPath+sPathSepUnix;
			if(acfg::stupidfs)
				UrlUnescapeAppend(dp->d_name, childbuf.sPath);
			else
				childbuf.sPath+=dp->d_name;
			bRet=childbuf.Walk(h, pFilter, bFollowSymlinks);
			if(!bRet)
				goto stop_walk;
		}
	}
	
	
//	cerr << "Closing: " << sPath<<endl;
	stop_walk:
	if(dir)
		closedir(dir);

	return h->ProcessDirAfter(sPath, m_stinfo) && bRet;
}



bool IFileHandler::DirectoryWalk(const string & sRoot, IFileHandler *h, bool bFilterDoubleDirVisit,
		bool bFollowSymlinks)
{
	dnode root(nullptr);
	dnode::tDupeFilter filter;
	root.sPath=sRoot; 
	return root.Walk(h, bFilterDoubleDirVisit ? &filter : nullptr, bFollowSymlinks);
}

// XXX: create some shortcut? wasting CPU cycles for virtual call PLUS std::function wrapper
bool IFileHandler::FindFiles(const mstring & sRootDir, IFileHandler::output_receiver callBack, bool bFilterDoubleDirVisit,
		bool bFollowSymlinks)
{
	struct tFileGrabber : public IFileHandler
	{
		IFileHandler::output_receiver &m_cb;
		bool ProcessRegular(cmstring &sPath, const struct stat &st) override { return m_cb(sPath, st);}
		bool ProcessOthers(cmstring &sPath, const struct stat &) override {return true;};
		bool ProcessDirAfter(cmstring &sPath, const struct stat &) override {return true;};
		tFileGrabber(IFileHandler::output_receiver &ret) : m_cb(ret) {}
	} cb(callBack);
	return DirectoryWalk(sRootDir, &cb, bFilterDoubleDirVisit, bFollowSymlinks);
}

