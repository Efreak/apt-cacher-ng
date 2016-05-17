
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"
#include "fileio.h"
#include "acbuf.h"
#include "acfg.h"

#ifdef HAVE_LINUX_FALLOCATE
#include <linux/falloc.h>
#include <fcntl.h>

using namespace std;

int falloc_helper(int fd, off_t start, off_t len)
{
   return fallocate(fd, FALLOC_FL_KEEP_SIZE, start, len);
}
#else
int falloc_helper(int, off_t, off_t)
{
   return 0;
}
#endif

// linking not possible? different filesystems?
bool FileCopy_generic(cmstring &from, cmstring &to)
{
	acbuf buf;
	buf.setsize(50000);
	int in(-1), out(-1);

	in=::open(from.c_str(), O_RDONLY);
	if (in<0) // error, here?!
		return false;

	while (true)
	{
		int err;
		err=buf.sysread(in);
		if (err<0)
		{
			if (err==-EAGAIN || err==-EINTR)
				continue;
			else
				goto error_copying;
		}
		else if (err==0)
			break;
		// don't open unless the input is readable, for sure
		if (out<0)
		{
			out=open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 00644);
			if (out<0)
				goto error_copying;
		}
		err=buf.syswrite(out);
		if (err<=0)
		{
			if (err==-EAGAIN || err==-EINTR)
				continue;
			else
				goto error_copying;
		}

	}

	forceclose(in);
	forceclose(out);
	return true;

	error_copying:

	checkforceclose(in);
	checkforceclose(out);
	return false;
}

/*
#if defined(HAVE_LINUX_SPLICE) && defined(HAVE_PREAD)
bool FileCopy(cmstring &from, cmstring &to)
{
	int in(-1), out(-1);

	in=::open(from.c_str(), O_RDONLY);
	if (in<0) // error, here?!
		return false;

	// don't open target unless the input is readable, for sure
	uint8_t oneByte;
	ssize_t err = pread(in, &oneByte, 1, 0);
	if (err < 0 || (err == 0 && errno != EINTR))
	{
		forceclose(in);
		return false;
	}
	out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 00644);
	if (out < 0)
	{
		forceclose(in);
		return false;
	}


	return FileCopy_generic(from, to);
}
#endif
*/



ssize_t sendfile_generic(int out_fd, int in_fd, off_t *offset, size_t count)
{
	char buf[8192];
	ssize_t totalcnt=0;
	
	if(!offset)
	{
		errno=EFAULT;
		return -1;
	}
	if(lseek(in_fd, *offset, SEEK_SET)== (off_t)-1)
		return -1;
	while(count>0)
	{
		auto readcount=read(in_fd, buf, std::min(count, sizeof(buf)));
		if(readcount<=0)
		{
			if(errno==EINTR || errno==EAGAIN)
				continue;
			else
				return readcount;
		}
		
		*offset+=readcount;
		totalcnt+=readcount;
		count-=readcount;
		
		for(decltype(readcount) nPos(0);nPos<readcount;)
		{
			auto r=write(out_fd, buf+nPos, readcount-nPos);
			if(r==0) continue; // not nice but needs to deliver it
			if(r<0)
			{
				if(errno==EAGAIN || errno==EINTR)
					continue;
				return r;
			}
			nPos+=r;
		}
	}
	return totalcnt;
}


bool xtouch(cmstring &wanted)
{
	mkbasedir(wanted);
	int fd = open(wanted.c_str(), O_WRONLY|O_CREAT|O_NOCTTY|O_NONBLOCK, acfg::fileperms);
	if(fd == -1)
		return false;
	checkforceclose(fd);
	return true;
}

void mkbasedir(const string & path)
{
	if(0==mkdir(GetDirPart(path).c_str(), acfg::dirperms) || EEXIST == errno)
		return; // should succeed in most cases

	// assuming the cache folder is already there, don't start from /, if possible
	unsigned pos=0;
	if(startsWith(path, acfg::cacheDirSlash))
	{
		// pos=acfg::cachedir.size();
		pos=path.find("/", acfg::cachedir.size()+1);
	}
    for(; pos<path.size(); pos=path.find(SZPATHSEP, pos+1))
    {
        if(pos>0)
            mkdir(path.substr(0,pos).c_str(), acfg::dirperms);
    }
}

void mkdirhier(cmstring& path)
{
	if(0==mkdir(path.c_str(), acfg::dirperms) || EEXIST == errno)
		return; // should succeed in most cases
	if(path.empty())
		return;
	for(string::size_type pos = path[0] == '/' ? 1 : 0;pos < path.size();pos++)
	{
		pos = path.find('/', pos);
		mkdir(path.substr(0,pos).c_str(), acfg::dirperms);
	}
}
