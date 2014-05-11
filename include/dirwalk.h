#ifndef DIRWALK_H_
#define DIRWALK_H_

#include "config.h"
#include <string>

#include <sys/stat.h>

class IFileHandler
{
public:
	virtual bool ProcessRegular(const MYSTD::string &sPath, const struct stat &) =0;
	virtual bool ProcessOthers(const MYSTD::string &sPath, const struct stat &)=0;
	virtual bool ProcessDirAfter(const MYSTD::string &sPath, const struct stat &)=0;
	virtual ~IFileHandler() {};
};

bool DirectoryWalk(const mstring & sRootDir, IFileHandler *h, bool bFilterDoubleDirVisit=true,
		bool bFollowSymlinks=true);




#endif /*DIRWALK_H_*/
