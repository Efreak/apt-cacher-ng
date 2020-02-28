/*
 * fileio.h
 *
 *  Created on: 25.07.2010
 *      Author: ed
 */

#ifndef FILEIO_H_
#define FILEIO_H_

#include "meta.h"

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <cstdio>
#include <unistd.h>

#ifdef HAVE_LINUX_SENDFILE
#include <sys/sendfile.h>
#endif

#ifndef O_BINARY
#define O_BINARY 0 // ignore on Unix
#endif

namespace acng
{

int falloc_helper(int fd, off_t start, off_t len);

ssize_t sendfile_generic(int out_fd, int in_fd, off_t *offset, size_t count);

class Cstat : public stat
{
	bool bResult;
public:
	inline Cstat(cmstring &s) { bResult = !::stat(s.c_str(), static_cast<struct stat*>(this)); }
	inline operator bool() const { return bResult; }
};

bool FileCopy_generic(cmstring &from, cmstring &to);

// in fact, pipe&splice&splice method works about 10% but only without considering other IO costs
// with them, the benefit is neglible

//#if defined(HAVE_LINUX_SPLICE) && defined(HAVE_PREAD)
//bool FileCopy(cmstring &from, cmstring &to);
//#else
#define FileCopy(x,y) FileCopy_generic(x,y)
//#endif

bool LinkOrCopy(const mstring &from, const mstring &to);


void set_nb(int fd);
void set_block(int fd);

inline void forceclose(int& fd) { while(0 != ::close(fd)) { if(errno != EINTR) break; }; fd=-1; }
void ACNG_API justforceclose(int fd);
inline void checkforceclose(int &fd)
{
	if (fd == -1)
		return;
	while (0 != ::close(fd))
	{
		if (errno != EINTR)
			break;
	};
	fd = -1;
}


inline void checkForceFclose(FILE* &fh)
{
	if (!fh)
		return;
	int fd = fileno(fh);
	if (0 != ::fclose(fh) && errno != EBADF)
	{
		forceclose(fd);
	}
	fh = nullptr;
}

// more efficient than tDtorEx with lambda
struct FILE_RAII
{
	FILE *p = nullptr;
	inline FILE_RAII() {};
	inline ~FILE_RAII() { close(); }
	operator FILE* () const { return p; }
	inline void close() { checkForceFclose(p); };
private:
	FILE_RAII(const FILE_RAII&);
	FILE_RAII operator=(const FILE_RAII&);
};

void mkdirhier(cmstring& path);
bool xtouch(cmstring &wanted);
void mkbasedir(const mstring & path);

/*
class tLazyStat
{
	LPCSTR path;
	struct stat stbuf;
	inline bool AccessFile() { if(path)
public:
	inline tLazyStat(LPCSTR p) : path(p) {};
	operator bool() const;
	off_t GetSize() const;
	off_t GetSpace() const;
};
*/

// open and/or create file for storage with the flags from our config
int open4write(cmstring &sPath);

}

#endif /* FILEIO_H_ */
