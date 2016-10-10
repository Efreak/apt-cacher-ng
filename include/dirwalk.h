#ifndef DIRWALK_H_
#define DIRWALK_H_

#include "config.h"
#include <string>

#include <sys/stat.h>

namespace acng
{

class IFileHandler
{
public:
	virtual bool ProcessRegular(const std::string &sPath, const struct stat &) =0;
	virtual bool ProcessOthers(const std::string &sPath, const struct stat &)=0;
	virtual bool ProcessDirAfter(const std::string &sPath, const struct stat &)=0;
	// noop, rarely used
	virtual bool ProcessLink(const std::string &sPath, const struct stat &) { return true;};
	virtual ~IFileHandler() {};

	typedef std::function<bool(cmstring &, const struct stat&)> output_receiver;

	static bool DirectoryWalk(const mstring & sRootDir, IFileHandler *h, bool bFilterDoubleDirVisit=true,
			bool bFollowSymlinks=true, bool bReportLinks=false);
	// similar, but returns only files, and just using a function pointer for that
	static bool FindFiles(const mstring & sRootDir, IFileHandler::output_receiver callBack,
			bool bFilterDoubleDirVisit=true,
			bool bFollowSymlinks=true);
};

}

#endif /*DIRWALK_H_*/
