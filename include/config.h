
#ifndef __CONFIG_H_
#define __CONFIG_H_

#include "acsyscap.h"

// safe fallbacks, should be defined by build system
#ifndef ACVERSION
#define ACVERSION "0.custom"
#endif
#ifndef CFGDIR
#define CFGDIR "/usr/local/etc/apt-cacher-ng"
#endif
#ifndef LIBDIR
#define LIBDIR "/usr/local/lib/apt-cacher-ng"
#endif

#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <climits>
#include <memory>

// added in Makefile... #define _FILE_OFFSET_BITS 64

namespace acng
{

#define SHARED_PTR std::shared_ptr
#define INTRUSIVE_PTR std::intrusive_ptr
#define WEAK_PTR std::weak_ptr
#define SCOPED_PTR std::auto_ptr

#ifdef NO_EXCEPTIONS
#define MYTRY
#define MYCATCH(x) if(false)
#else
#define MYTRY try
#define MYCATCH catch
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define ENEMIESOFDOSFS "?[]\\=+<>:;#"

//! Time after which the pooled sockets are considered EOLed
#define TIME_SOCKET_EXPIRE_CLOSE 33

#define COMMA ,
#ifdef HAVE_SSL
#define IFSSLORFALSE(x) x
#define SSL_OPT_ARG(x) COMMA x
#else
#define IFSSLORFALSE(x) false
#define SSL_OPT_ARG(x)
#endif

}
#endif // __CONFIG_H
