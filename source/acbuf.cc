
//#define LOCAL_DEBUG
#include "debug.h"

#include "config.h"

#include "acbuf.h"
#include "fileio.h"
#include <unistd.h>

bool acbuf::setsize(unsigned int c) {
	if(m_nCapacity==c)
		return true;

	char *p = (char*) realloc(m_buf, c+1);
	if(!p)
		return false;

	m_buf=p;
	m_nCapacity=c;
	m_buf[c]=0; // terminate to make string operations safe
    return true;
}

bool acbuf::initFromFile(const char *szPath)
{
	struct stat statbuf;

	if (0!=stat(szPath, &statbuf))
		return false;

	int fd=::open(szPath, O_RDONLY);
	if (fd<0)
		return false;

	clear();

	if(!setsize(statbuf.st_size))
		return false;
	
	while (freecapa()>0)
	{
		if (sysread(fd) < 0)
		{
			forceclose(fd);
			return false;
		}
	}
	forceclose(fd);
	return true;
}

int acbuf::syswrite(int fd, unsigned int maxlen) {
    size_t todo(std::min(maxlen, size()));

	int n;
	do
	{
		n=::write(fd, rptr(), todo);
	} while(n<0 && errno==EINTR);
	
	if(n<0 && errno==EAGAIN)
		n=0;
    if(n<0)
        return -errno;
    drop(n);
    return n;
}

int acbuf::sysread(int fd, unsigned int maxlen)
{
	size_t todo(std::min(maxlen, freecapa()));
	int n;
	do {
		n=::read(fd, m_buf+w, todo);
	} while( (n<0 && EINTR == errno) /* || (EAGAIN == errno && n<=0) */ ); // cannot handle EAGAIN here, let the caller check errno
    if(n<0)
    	return -errno;
    if(n>0)
        w+=n;
    return(n);
}

bool tSS::send(int nConFd, mstring& sErrorStatus)
{
	while (!empty())
	{
		auto n = ::send(nConFd, rptr(), size(), 0);
		if (n > 0)
		{
			drop(n);
			continue;
		}
		if (n <= 0)
		{
			if (EINTR == errno || EAGAIN == errno)
			{
				struct timeval tv{acfg::nettimeout, 0};
				fd_set wfds;
				FD_ZERO(&wfds);
				FD_SET(nConFd, &wfds);
				auto r=::select(nConFd + 1, nullptr, &wfds, nullptr, &tv);
				if(!r && errno != EINTR)
				{
					sErrorStatus = "502 Socket timeout";
					return false;
				}
				continue;
			}

#ifdef MINIBUILD
			sErrorStatus = "502 Socket error";
#else
			sErrorStatus = tErrnoFmter("502 Socket error, ");
#endif
			return false;
		}
	}
	return true;
}

bool tSS::recv(int nConFd, mstring& sErrorStatus)
{
	struct timeval tv
	{ acfg::nettimeout, 0 };
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(nConFd, &rfds);
	auto r = ::select(nConFd + 1, &rfds, nullptr, nullptr, &tv);
	if (!r)
	{
		if(errno == EINTR)
			return true;

		sErrorStatus = "502 Socket timeout";
		return false;
	}
	// must be readable
	r = ::recv(nConFd, wptr(), freecapa(), 0);
	if(r<=0)
	{
#ifdef MINIBUILD
			sErrorStatus = "502 Socket error";
#else
			sErrorStatus = tErrnoFmter("502 Socket error, ");
#endif
			return false;
	}
	got(r);
	return true;
}

/*
tSS & tSS::addEscaped(const char *fmt)
{
	if(!fmt || !*fmt)
		return *this;
	int nl=strlen(fmt);
	reserve(length()+nl);
	char *p=wptr();

	for(;*fmt;fmt++)
	{
		if(*fmt=='\\')
			*(p++)=unEscape(*(++fmt));
		else
			*(p++)=*fmt;
	}
	got(p-wptr());
	return *this;
}
*/
