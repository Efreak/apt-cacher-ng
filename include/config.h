
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

#if defined _WIN32 || defined __CYGWIN__
  #define ACNG_SO_IMPORT __declspec(dllimport)
  #define ACNG_SO_EXPORT __declspec(dllexport)
  #define ACNG_SO_LOCAL
#else
  #if __GNUC__ >= 4
    #define ACNG_SO_IMPORT __attribute__ ((visibility ("default")))
    #define ACNG_SO_EXPORT __attribute__ ((visibility ("default")))
    #define ACNG_SO_LOCAL  __attribute__ ((visibility ("hidden")))
  #else
    #define ACNG_SO_IMPORT
    #define ACNG_SO_EXPORT
    #define ACNG_SO_LOCAL
  #endif
#endif

#ifdef ACNG_CORE_IN_SO
  #ifdef supacng_EXPORTS // defined by cmake for shared lib project
    #define ACNG_API ACNG_SO_EXPORT
  #else
    #define ACNG_API ACNG_SO_IMPORT
  #endif // ACNG_DLL_EXPORTS
  #define ACNG_LOCAL ACNG_SO_LOCAL
#else // ACNG_DLL is not defined, code is built in as usual
  #define ACNG_API
  #define ACNG_LOCAL
#endif // ACNG_DLL

}

#if __cplusplus >= 201703L
#define IS_CXX17
#endif

#if __cplusplus >= 201402L
#define IS_CXX14
#endif

//#ifdef UNDER_TEST
//#include "sut_config.h"
//#endif

#endif // __CONFIG_H
