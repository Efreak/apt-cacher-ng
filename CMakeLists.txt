cmake_minimum_required(VERSION 3.1)

# enforce minimal C++ language level
set(CMAKE_CXX_STANDARD 14)
# let it take the lowest version, we need some precursor of C++14x
#set(CMAKE_CXX_STANDARD_REQUIRED on)

# don't care for now... just expecting POSIX
set(CMAKE_LEGACY_CYGWIN_WIN32 0)

set(PACKAGE "apt-cacher-ng")

PROJECT(${PACKAGE} CXX C)

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR})
set(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR})
#set(CMAKE_SKIP_BUILD_RPATH on) # no -rdynamic needed ever

INCLUDE(CheckIncludeFiles) 
INCLUDE(CheckCXXSourceCompiles)
INCLUDE(CheckCXXCompilerFlag)
INCLUDE(CheckTypeSize)
INCLUDE(TestBigEndian)
INCLUDE(CheckLibraryExists)
INCLUDE(CheckSymbolExists)
INCLUDE(FindPkgConfig)
INCLUDE(GNUInstallDirs)

IF(NOT DEFINED(CMAKE_INSTALL_PREFIX))
set(CMAKE_INSTALL_PREFIX "/usr/local" CACHE STRING "Target file space")
ENDIF()
IF(NOT DEFINED(LIBDIR))
	set(LIBDIR "${CMAKE_INSTALL_PREFIX}/lib/${PACKAGE}" CACHE STRING "Location of ${PACKAGE} extra files")
ENDIF()
if(NOT DEFINED(CFGDIR))
	set(CFGDIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/${PACKAGE}")
endif()
if(NOT DEFINED(RUNDIR))
	set(RUNDIR "/run")
endif()

IF(NOT DEFINED(ACNG_CACHE_DIR))
        set(ACNG_CACHE_DIR "/var/tmp" CACHE STRING "Cache folder for examples and default configuration")
ENDIF()

IF(NOT DEFINED(ACNG_LOG_DIR))
        set(ACNG_LOG_DIR "/var/tmp" CACHE STRING "Log file folder for examples and default configuration")
ENDIF()

# carefully splicing of command line arguments, even from lists
macro(_append varname)
        string(REPLACE ";" " " _apx "${ARGN}")
        if(NOT DEFINED ${varname})
                set(${varname} "${_apx}")
        else()
                set(${varname} "${${varname}} ${_apx}")
        endif()
endmacro()

INCLUDE_DIRECTORIES(${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR} "include")

IF(NOT DEFINED(ACVERSION))
FILE(READ "${CMAKE_SOURCE_DIR}/VERSION" ACVERSION)
string(REPLACE "\n" "" ACVERSION ${ACVERSION})
#MESSAGE(STATUS "Version: ${ACVERSION}")
ENDIF()

# Various feature checks
CHECK_INCLUDE_FILES ("sys/param.h;sys/mount.h" HAVE_SYS_MOUNT_H)
CHECK_INCLUDE_FILES ("sys/vfs.h" HAVE_SYS_VFS_H)
CHECK_TYPE_SIZE(int SIZE_INT)
CHECK_TYPE_SIZE(long SIZE_LONG)
TEST_BIG_ENDIAN(WORDS_BIGENDIAN)

# those are appended later via COMPILER_FLAGS. $CXXFLAGS and CMAKE_CXX_FLAGS
# also influence it, need to be adjusted carefully by user.
SET(ACNG_COMPFLAGS "-D_FILE_OFFSET_BITS=64")

find_package(Threads REQUIRED)
if(THREADS_HAVE_PTHREAD_ARG)
        _append(CFLAGS_PTHREAD -pthread)
endif()

foreach(cxxarg -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden)
	STRING(REGEX REPLACE "=|-|," "" optname "${cxxarg}")
  CHECK_CXX_COMPILER_FLAG("${cxxarg}" "COPT_${optname}")
  if(COPT_${optname})
          _append(ACNG_COMPFLAGS ${cxxarg})
  endif()
endforeach()

CHECK_CXX_COMPILER_FLAG(-fvisibility-inlines-hidden CXX_VIHI)
if(CXX_VIHI)
        _append(ACNG_CXXFLAGS -fvisibility-inlines-hidden)
endif()

foreach(linkarg -Wl,--as-needed -Wl,-O1 -Wl,--discard-all -Wl,--no-undefined -Wl,--build-id=sha1 -Wl,-fuse-ld=gold)
	STRING(REGEX REPLACE "=|-|," "" optname "${linkarg}")
	set(CMAKE_REQUIRED_FLAGS "${linkarg}")
	CHECK_CXX_COMPILER_FLAG("" "LD_${optname}")
	if(LD_${optname})
          _append(CMAKE_EXE_LINKER_FLAGS ${linkarg})
	endif()
	set(CMAKE_REQUIRED_FLAGS "")
endforeach(linkarg)

set(CMAKE_REQUIRED_FLAGS "-Wl,-fuse-ld=gold -Wl,--threads")
CHECK_CXX_COMPILER_FLAG("" LD_MULTITHREADED)
if(LD_MULTITHREADED)
	_append(CMAKE_EXE_LINKER_FLAGS "-Wl,-fuse-ld=gold -Wl,--threads")
endif()
set(CMAKE_REQUIRED_FLAGS "")

option(USE_SSL "Use OpenSSL library for TLS and other crypto functionality" on)

IF(CMAKE_SYSTEM MATCHES "Darwin")
        _append(ACNG_COMPFLAGS -D_DARWIN_C_SOURCE)
ENDIF()

IF(CMAKE_SYSTEM MATCHES "CYGWIN")
	set(USE_LTO_DEFAULT off)
  _append(ACNG_COMPFLAGS -U__STRICT_ANSI__ -DNOMINMAX)
ELSE()
  if(CMAKE_COMPILER_IS_GNUCXX AND CMAKE_CXX_COMPILER_VERSION MATCHES "^[56789]")
     set(USE_LTO_DEFAULT on)
  endif()
ENDIF()

# set the default to OFF for now because gold is still too buggy
set(USE_LTO_DEFAULT off)

if(CMAKE_BUILD_TYPE MATCHES Debug)
   set(USE_LTO_DEFAULT off)
   _append(ACNG_COMPFLAGS -DDEBUG)
else()
   set(CMAKE_REQUIRED_FLAGS "-Wl,--gc-sections")
   CHECK_CXX_COMPILER_FLAG("-Os -fdata-sections -ffunction-sections -Wl,--gc-sections" GC_SECTIONS)
   if(GC_SECTIONS)
           _append(ACNG_COMPFLAGS -fdata-sections -ffunction-sections)
           _append(CMAKE_EXE_LINKER_FLAGS -Wl,--gc-sections)
           _append(CMAKE_SHARED_LINKER_FLAGS -Wl,--gc-sections)
   endif()
   set(CMAKE_REQUIRED_FLAGS "")
endif()

option(USE_LTO "Enable Link Time Optimization (requires modern compilers)" ${USE_LTO_DEFAULT})

if(USE_LTO)
        SET(LDFLAGS_BACKUP "${CMAKE_EXE_LINKER_FLAGS}")
        SET(CMAKE_REQUIRED_FLAGS "${ACNG_COMPFLAGS} -flto")
        _append(CMAKE_EXE_LINKER_FLAGS -flto)
	CHECK_CXX_SOURCE_COMPILES("int main() {return 0;}" HAS_LTO)
        if(HAS_LTO)
                SET(ACNG_COMPFLAGS ${CMAKE_REQUIRED_FLAGS})
        else()
                SET(CMAKE_REQUIRED_FLAGS "${ACNG_COMPFLAGS}")
                SET(CMAKE_EXE_LINKER_FLAGS "${LDFLAGS_BACKUP}")
                message(WARNING "Link Time Optimization support broken, disabling it.")
		set(USE_LTO off)
        endif()
endif()

FIND_LIBRARY(HAVE_SOCKETLIB socket) # separate socket lib looks like Solaris-like environment
if(HAVE_SOCKETLIB)
   LIST(APPEND BaseNetworkLibs socket nsl)
endif(HAVE_SOCKETLIB)

SET(CMAKE_REQUIRED_LIBRARIES wrap ${BaseNetworkLibs})
FILE(READ test/build/HAVE_LIBWRAP.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_LIBWRAP)
IF(HAVE_LIBWRAP)
   LIST(APPEND ServerLibs wrap)
ELSE(HAVE_LIBWRAP)
   MESSAGE("!! libwrap development files not usable, disabling support")
   SET(HAVE_LIBWRAP)
ENDIF(HAVE_LIBWRAP)
SET(CMAKE_REQUIRED_LIBRARIES "")

# lib is not in the standard path so FIND_LIBRARY is not reliable
set(CMAKE_REQUIRED_FLAGS "-latomic")
CHECK_CXX_COMPILER_FLAG("" LD_ATOMIC)
if(LD_ATOMIC)
_append(BaseNetworkLibs atomic)
endif()
set(CMAKE_REQUIRED_FLAGS "")

if(CYGWIN)
message("!! Not using wordexp on Cygwin, not reliable")
set(HAVE_WORDEXP off)
else()
FILE(READ test/build/HAVE_WORDEXP.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_WORDEXP)
endif()

# test some methods explicitly where we want to be sure about the signatures
# and linkability

FILE(READ test/build/HAVE_GLOB.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_GLOB)

FILE(READ test/build/HAVE_FADVISE.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_FADVISE)

FILE(READ test/build/HAVE_MADVISE.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_MADVISE)

if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")

   FILE(READ test/build/HAVE_LINUX_FALLOCATE.cc TESTSRC)
   CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_LINUX_FALLOCATE)

   FILE(READ test/build/HAVE_LINUX_SENDFILE.cc TESTSRC)
   CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_LINUX_SENDFILE)

   FILE(READ test/build/HAVE_LINUX_EVENTFD.cc TESTSRC)
   CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_LINUX_EVENTFD)

   # FILE(READ test/build/HAVE_LINUX_SPLICE.cc TESTSRC)
   # CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_LINUX_SPLICE)
endif()

FILE(READ test/build/HAVE_PREAD.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_PREAD)

FILE(READ test/build/HAVE_DAEMON.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_DAEMON)

pkg_check_modules(lsd "libsystemd>=209")
# either part of the big library nowadays or in the helper library on older systems
if(NOT lsd_FOUND)
pkg_check_modules(lsd libsystemd-daemon)
endif()
_append(CFLAGS_DAEMON ${lsd_CFLAGS})
_append(LDFLAGS_DAEMON ${lsd_LDFLAGS})
set(HAVE_SD_NOTIFY ${lsd_FOUND})

SET(CMAKE_REQUIRED_LIBRARIES dl)
FILE(READ test/build/HAVE_DLOPEN.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_DLOPEN)

# XXX: convert the compressor stuff to pkgconfig usage and print an overview of enabled/found values in one final message and collect and process cflags too

SET(CMAKE_REQUIRED_FLAGS ${ACNG_COMPFLAGS})
#INCLUDE(FindZLIB) # broken, hangs for 10 seconds
# header check should be enough, gzip should be everywhere nowadays
#CHECK_INCLUDE_FILES("gzip.h" HAVE_ZLIB)
FIND_PATH(HAVE_ZLIB zlib.h )
if(HAVE_ZLIB)
	list(APPEND CompLibs z)
	INCLUDE_DIRECTORIES(${HAVE_ZLIB})
else(HAVE_ZLIB)
   message(FATAL_ERROR "!! apt-cacher-ng requires gzip library and development files ${HAVE_ZLIB}")
endif(HAVE_ZLIB)

INCLUDE(FindBZip2)
if (BZIP2_FOUND)
   SET(HAVE_LIBBZ2 1)
   MARK_AS_ADVANCED(HAVE_LIBBZ2)
	INCLUDE_DIRECTORIES(${BZIP2_INCLUDE_DIR})
	list(APPEND CompLibs bz2)
else (BZIP2_FOUND)
   message("!! apt-cacher-ng requires bzip2 library and development files for bz2 format support")
endif (BZIP2_FOUND)

SET(CMAKE_REQUIRED_FLAGS ${ACNG_COMPFLAGS})
SET(CMAKE_REQUIRED_LIBRARIES lzma)
FILE(READ test/build/HAVE_LZMA.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_LZMA)
IF(HAVE_LZMA)
	list(APPEND CompLibs lzma)
ELSE(HAVE_LZMA)
   MESSAGE("!! XZ (liblzma) not found or not working, disabling support")
   SET(HAVE_LZMA )
ENDIF(HAVE_LZMA)
SET(CMAKE_REQUIRED_LIBRARIES "")

set(HAVE_CHECKSUM on)

pkg_check_modules(libevent libevent)
if(NOT libevent_FOUND)
        message(FATAL_ERROR "!! Error: proper libevent installation is required")
else()
        string(REPLACE ";" " " ACNG_COMPFLAGS "${ACNG_COMPFLAGS} ${libevent_CFLAGS}")
        LIST(APPEND BaseNetworkLibs ${libevent_LDFLAGS})
endif()


pkg_check_modules(libevent_pthreads libevent_pthreads)
if(NOT libevent_pthreads_FOUND)
        message(FATAL_ERROR "!! Error: proper libevent_pthreads installation is required")
else()
        string(REPLACE ";" " " ACNG_COMPFLAGS "${ACNG_COMPFLAGS} ${libevent_pthreads_CFLAGS}")
        LIST(APPEND BaseNetworkLibs ${libevent_pthreads_LDFLAGS})
endif()

if(USE_SSL)
	pkg_check_modules(openssl "openssl>=1.0.2")
	if(openssl_FOUND)
		set(HAVE_SSL on)
    _append(ACNG_COMPFLAGS ${openssl_CFLAGS})
    _append(SSL_LIB_LIST ${openssl_LDFLAGS})
	else()
		message(WARNING "OpenSSL not found, disabling TLS connection support")
		set(HAVE_SSL off)
	endif()
endif()

if(NOT HAVE_SSL)
if(EXISTS "$ENV{TOMCRYPT_HOME}")
message("!! TOMCRYPT_HOME folder found, will use the library from there")
include_directories("$ENV{TOMCRYPT_HOME}/src/headers")
link_directories("$ENV{TOMCRYPT_HOME}")
link_libraries(tomcrypt)
set(tomcrypt_FOUND on)
else()
	pkg_check_modules(tomcrypt "libtomcrypt")
endif()

	if(tomcrypt_FOUND)
		set(HAVE_TOMCRYPT on)
		# with workaround, http://clang.debian.net/logs/2015-03-25/libtomcrypt_1.17-6_unstable_clang.log
    _append(ACNG_COMPFLAGS ${tomcrypt_CFLAGS} -DLTC_NO_ROLC -DLTC_BASE64)
    _append(SSL_LIB_LIST ${tomcrypt_LDFLAGS})
	else()
		set(HAVE_TOMCRYPT off)
		message(WARNING "Could not find LibTomCrypt or use OpenSSL. Some important functionality will be disabled.")
		set(HAVE_CHECKSUM off)
	endif()
endif()

# -DEXTRA_LIBS_INETD=-lsupc++
# funny hack, link with gcc and avoid libstdc++/libm (since STL is not used
# there). However, it needs to be tested - the alternative linking makes
# the binary ~40kb larger, might include higher relocation costs and bigger
# chunks of unique memory while libstdc++ needs to be loaded anyway for the
# server process.
# Needs HAVE_WL_AS_NEEDED!

message("Build settings:")
message("Compiler flags (common): ${ACNG_COMPFLAGS}")
message("Compiler flags (C++ only): ${ACNG_CXXFLAGS}")
message("Compiler flags (environment): ${CMAKE_CXX_FLAGS}")
message("Linker flags: ${CMAKE_EXE_LINKER_FLAGS}")
message("LTO use: ${USE_LTO}
")

# unset everything, only use as needed
SET(CMAKE_REQUIRED_LIBRARIES )
SET(CMAKE_REQUIRED_FLAGS )

# I don't need -rdynamic, thanks!
SET(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")
SET(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")

#######################################
# all checks done, save configuration #
#######################################

CONFIGURE_FILE("${CMAKE_SOURCE_DIR}/include/acsyscap.h.in" "${CMAKE_BINARY_DIR}/acsyscap.h")

add_subdirectory(client)
add_subdirectory(fs)
add_subdirectory(source)
add_subdirectory(conf)
add_subdirectory(systemd)

###
### Extra install rules for static files
###
install(FILES doc/README doc/${PACKAGE}.pdf DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY doc/html/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/html
   FILES_MATCHING PATTERN "*.*")
install(DIRECTORY doc/man/ DESTINATION ${CMAKE_INSTALL_MANDIR}/man8
   FILES_MATCHING PATTERN "*.8")
if(NOT DEFINED(AVAHIDIR))
        set(AVAHIDIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/avahi/services")
endif()
install(FILES contrib/apt-cacher-ng.service DESTINATION ${AVAHIDIR})

message("Installation settings:
PACKAGE: ${PACKAGE}
VERSION: ${ACVERSION}
CMAKE_INSTALL_PREFIX: ${CMAKE_INSTALL_PREFIX}
CMAKE_INSTALL_FULL_SYSCONFDIR: ${CMAKE_INSTALL_FULL_SYSCONFDIR}
LIBDIR: ${LIBDIR}
AVAHIDIR: ${AVAHIDIR}
")
