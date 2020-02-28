/*
 * errnofmt.cc
 *
 *  Created on: 28.02.2020
 *      Author: Eduard Bloch
 */

#include "meta.h"
#include "acbuf.h"

#include <unistd.h>
#include <string.h>
#include <errno.h>

namespace acng
{


// let the compiler decide between GNU and XSI version
inline void add_msg(int r, int err, const char* buf, mstring *p)
{
	if(r)
		p->append(tSS() << "UNKNOWN ERROR: " << err);
	else
		p->append(buf);
}

inline void add_msg(const char *msg, int , const char* , mstring *p)
{
	p->append(msg);
}


tErrnoFmter::tErrnoFmter(const char *prefix)
{
	int err=errno;
	char buf[64];
	buf[0]=buf[sizeof(buf)-1]=0x0;
	if(prefix)
		assign(prefix);
	add_msg(strerror_r(err, buf, sizeof(buf)-1), err, buf, this);
}

tErrnoFmter::tErrnoFmter(const char *prefix, int err)
{
	char buf[64];
	buf[0]=buf[sizeof(buf)-1]=0x0;
	if(prefix)
		assign(prefix);
	add_msg(strerror_r(err, buf, sizeof(buf)-1), err, buf, this);
}
}
