
#ifndef __CONFIG_H_
#define __CONFIG_H_

#include "acsyscap.h"

#define ACVERSION "0.8.0rc4"

#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <climits>

#ifdef HAVE_MEMORY_SPTR
#include <memory>
#define SMARTPTR_SPACE std
#elif defined HAVE_TR1_MEMORY
#include <tr1/memory>
#define SMARTPTR_SPACE std::tr1
#elif defined HAVE_BOOST_SMARTPTR
#include <boost/smart_ptr.hpp>
#define SMARTPTR_SPACE boost
#else
#error Unable to find smart pointer implementation, install Boost or recent compiler with STL containing TR1 components. Set BOOSTDIR in Makefile if needed.
#endif

// make off_t be a 64 bit type
// added in Makefile... #define _FILE_OFFSET_BITS 64

#define SHARED_PTR SMARTPTR_SPACE::shared_ptr
#define INTRUSIVE_PTR SMARTPTR_SPACE::intrusive_ptr
#define WEAK_PTR SMARTPTR_SPACE::weak_ptr
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

#endif // __CONFIG_H

