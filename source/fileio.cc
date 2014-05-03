
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"
#include "fileio.h"
#include "acbuf.h"

#ifdef HAVE_LINUX_FALLOCATE
#include <linux/falloc.h>
#include <fcntl.h>

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

