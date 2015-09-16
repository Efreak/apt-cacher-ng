set(CMAKE_LEGACY_CYGWIN_WIN32 0) # don't care for now... just expecting POSIX
cmake_minimum_required(VERSION 2.6.2)
PROJECT(ACNG CXX C)

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR})
set(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR})

# we want the old policy to support cmake 2.x
if(POLICY CMP0056) # added in CMake 3.2
   # setting linker flags explicitly
	cmake_policy(SET CMP0056 OLD)
endif()
IF (POLICY CMP0053) # added in CMake 3.1
	cmake_policy (SET CMP0053 OLD) # use old-style @VAR@ expansion
ENDIF (POLICY CMP0053)

INCLUDE(CheckIncludeFiles) 
INCLUDE(CheckCXXSourceCompiles)
INCLUDE(CheckCXXCompilerFlag)
INCLUDE(CheckTypeSize)
INCLUDE(TestBigEndian)
INCLUDE(CheckLibraryExists)
INCLUDE(FindPkgConfig)

set(TOOLNAME "apt-cacher-ng")
IF(NOT DEFINED(CMAKE_INSTALL_PREFIX))
set(CMAKE_INSTALL_PREFIX "/usr/local" CACHE STRING "Target file space")
ENDIF()
IF(NOT DEFINED(DATADIR))
	set(DATADIR "${CMAKE_INSTALL_PREFIX}/share" CACHE STRING "Shared data files directory")
ENDIF()
IF(NOT DEFINED(LIBDIR))
	set(LIBDIR "${CMAKE_INSTALL_PREFIX}/lib/${TOOLNAME}" CACHE STRING "Location of ${TOOLNAME} extra files")
ENDIF()
IF(NOT DEFINED(DOCDIR))
	set(DOCDIR "${DATADIR}/doc/${TOOLNAME}" CACHE STRING "Location of documentation files")
ENDIF()

IF(NOT DEFINED(SYSCONFDIR))
   if("${CMAKE_INSTALL_PREFIX}" STREQUAL "/usr")
      set(SYSCONFDIR /etc)
   else()
      set(SYSCONFDIR ${CMAKE_INSTALL_PREFIX}/etc)
   endif()
ENDIF()

if(NOT DEFINED(CFGDIR))
set(CFGDIR "${SYSCONFDIR}/${TOOLNAME}")
endif()

if(NOT DEFINED(SBINDIR))
set(SBINDIR "${CMAKE_INSTALL_PREFIX}/sbin")
endif()

INCLUDE_DIRECTORIES(${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR} "include")

IF(NOT DEFINED(ACVERSION))
FILE(READ "${CMAKE_SOURCE_DIR}/VERSION" ACVERSION)
string(REPLACE "\n" "" ACVERSION ${ACVERSION})
MESSAGE(STATUS "Version: ${ACVERSION}")
ENDIF()

# Various feature checks
CHECK_INCLUDE_FILES ("sys/param.h;sys/mount.h" HAVE_SYS_MOUNT_H)
CHECK_INCLUDE_FILES ("sys/vfs.h" HAVE_SYS_VFS_H)
CHECK_TYPE_SIZE(int SIZE_INT)
CHECK_TYPE_SIZE(long SIZE_LONG)
TEST_BIG_ENDIAN(WORDS_BIGENDIAN)

# those are appended later via COMPILER_FLAGS. $CXXFLAGS and CMAKE_CXX_FLAGS
# also influence it, need to be adjusted carefully by user.
SET(ACNG_CXXFLAGS_COMMON " -pthread -Wall -Wextra -Wno-unused-parameter -D_FILE_OFFSET_BITS=64 ")

foreach(linkarg -Wl,--as-needed -Wl,-O1 -Wl,--discard-all -Wl,--no-undefined -Wl,--build-id=sha1)
	STRING(REGEX REPLACE "=|-|," "" optname "${linkarg}")
	CHECK_CXX_COMPILER_FLAG("${linkarg}" "LD_${optname}")
	if(LD_${optname})
     SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${linkarg}")
	endif()
endforeach(linkarg)

option(USE_SSL "Use OpenSSL library for TLS and other crypto functionality" on)
# option(USE_LTO "Enable Link Time Optimization (requires modern compilers)" on)

IF(CMAKE_SYSTEM MATCHES "Darwin")
   SET(ACNG_CXXFLAGS_COMMON " ${ACNG_CXXFLAGS_COMMON} -D_DARWIN_C_SOURCE ")
ENDIF(CMAKE_SYSTEM MATCHES "Darwin")

IF(CMAKE_SYSTEM MATCHES "CYGWIN")
	set(USE_LTO off)
	SET(ACNG_CXXFLAGS_COMMON " ${ACNG_CXXFLAGS_COMMON}  -U__STRICT_ANSI__ -DNOMINMAX ")
ELSE()

ENDIF()

if(CMAKE_BUILD_TYPE MATCHES Debug)
	set(USE_LTO off)
	set(ACNG_CXXFLAGS_COMMON "-DDEBUG ${ACNG_CXXFLAGS_COMMON}")
endif()

FILE(READ test/build/testcxx11.cc CXX11_TESTSRC)
# prefer that style if possible since since Cygwin seems to have an old bug with missing definitions, http://cygwin.com/ml/cygwin/2012-04/msg00140.html
CHECK_CXX_COMPILER_FLAG(-std=gnu++11 COMPILER_SUPPORTS_GNU11)
if(COMPILER_SUPPORTS_GNU11)
	set(ACNG_CXXFLAGS_COMMON "${ACNG_CXXFLAGS_COMMON} -std=gnu++11")
else()
	CHECK_CXX_COMPILER_FLAG(-std=c++11 COMPILER_SUPPORTS_CXX11)
	if(COMPILER_SUPPORTS_CXX11)
		set(ACNG_CXXFLAGS_COMMON "${ACNG_CXXFLAGS_COMMON} -std=c++11")
	else()
		CHECK_CXX_COMPILER_FLAG(-std=c++0x COMPILER_SUPPORTS_CXX0X)
		if(COMPILER_SUPPORTS_CXX0X)
			set(ACNG_CXXFLAGS_COMMON "${ACNG_CXXFLAGS_COMMON} -std=c++0x")
		else()
			message(FATAL_ERROR "!! Error: failed to configure compiler for C++11 support. For GCC, version 4.7 or newer is required.")
		endif()
	endif()
endif()

if(USE_LTO)
   SET(LDFLAGS_NOLTO "${CMAKE_EXE_LINKER_FLAGS}")
   SET(CMAKE_REQUIRED_FLAGS "${ACNG_CXXFLAGS_COMMON} -flto -flto-partition=none")
   SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto -flto-partition=none")
   CHECK_CXX_SOURCE_COMPILES("${CXX11_TESTSRC}" HAS_LTO)
   if(HAS_LTO)
      SET(ACNG_CXXFLAGS_COMMON ${CMAKE_REQUIRED_FLAGS})
   else()
      SET(CMAKE_REQUIRED_FLAGS "${ACNG_CXXFLAGS_COMMON}")
      SET(CMAKE_EXE_LINKER_FLAGS "${LDFLAGS_NOLTO}")
      message(WARNING "Link Time Optimization support broken, disabling it.")
   endif()
endif()

LIST(APPEND NONOSSLSRCS "${CMAKE_SOURCE_DIR}/source/sha1.cc" "${CMAKE_SOURCE_DIR}/source/md5.cc" "${CMAKE_SOURCE_DIR}/source/sha2.cc")

# sources to strip from the globed list, to be added as needed
LIST(APPEND OPTSOURCES  "${CMAKE_SOURCE_DIR}/source/sha1.cc" "${CMAKE_SOURCE_DIR}/source/md5.cc" "${CMAKE_SOURCE_DIR}/source/sha2.cc")
# keep "${CMAKE_SOURCE_DIR}/source/rfc2553emu.cc" because auto-disables its implementation where unneeded

FIND_LIBRARY(HAVE_SOCKETLIB socket) # separate socket lib looks like Solaris-like environment
if(HAVE_SOCKETLIB)
   LIST(APPEND BaseNetworkLibs socket nsl)
endif(HAVE_SOCKETLIB)

# explicit linking needed for global locks in dlcon
list(APPEND BaseNetworkLibs pthread)

SET(CMAKE_REQUIRED_LIBRARIES wrap ${BaseNetworkLibs})
FILE(READ test/build/HAVE_LIBWRAP.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_LIBWRAP)
IF(HAVE_LIBWRAP)
	set(ServerLibs ${CMAKE_REQUIRED_LIBRARIES})
ELSE(HAVE_LIBWRAP)
   MESSAGE("!! libwrap development files not usable, disabling support")
   SET(HAVE_LIBWRAP)
ENDIF(HAVE_LIBWRAP)
SET(CMAKE_REQUIRED_LIBRARIES "")

if(CYGWIN)
message("!! Not using wordexp on Cygwin, not reliable")
set(HAVE_WORDEXP off)
else()
FILE(READ test/build/HAVE_WORDEXP.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_WORDEXP)
endif()

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

   FILE(READ test/build/HAVE_LINUX_SPLICE.cc TESTSRC)
   CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_LINUX_SPLICE)
endif()

FILE(READ test/build/HAVE_PREAD.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_PREAD)

FILE(READ test/build/HAVE_DAEMON.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_DAEMON)

pkg_check_modules(lsd "libsystemd>=209")
if(NOT lsd_FOUND)
pkg_check_modules(lsd libsystemd-daemon)
endif()
string(REPLACE ";" " " lsd_LDFLAGS "${lsd_LDFLAGS}")
string(REPLACE ";" " " lsd_CFLAGS "${lsd_CFLAGS}")
set(HAVE_SD_NOTIFY ${lsd_FOUND})

SET(CMAKE_REQUIRED_FLAGS ${ACNG_CXXFLAGS_COMMON})
FILE(READ test/build/HAVE_MEMORY_SPTR.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_MEMORY_SPTR)
if(NOT HAVE_MEMORY_SPTR)
   message(FATAL_ERROR "Could not find a working smart pointer implementation.")
endif()

SET(CMAKE_REQUIRED_LIBRARIES dl)
FILE(READ test/build/HAVE_DLOPEN.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_DLOPEN)

SET(CMAKE_REQUIRED_FLAGS ${ACNG_CXXFLAGS_COMMON})
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

SET(CMAKE_REQUIRED_FLAGS ${ACNG_CXXFLAGS_COMMON})
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

if(USE_SSL)
   SET(SSL_LIB_LIST ssl crypto)
   SET(CMAKE_REQUIRED_LIBRARIES "${SSL_LIB_LIST}")
   FILE(READ test/build/HAVE_SSL.cc TESTSRC)
   CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_SSL)
   IF(HAVE_SSL)
      SET(NONOSSLSRCS )
      # and keep SSL_LIB_LIST
   ELSE(HAVE_SSL)
      MESSAGE("!! OpenSSL not found or not working, disabling support")
      SET(SSL_LIB_LIST )
      message(WARNING "fixme, NONOSSLSRCS verwenden überall")
   ENDIF(HAVE_SSL)
endif()

# -DEXTRA_LIBS_INETD=-lsupc++
# funny hack, link with gcc and avoid libstdc++/libm (since no STL parts used
# there). However, it needs to be investigated - the alternative linking makes
# the binary ~40kb larger, might include higher relocation costs and bigger
# chunks of unique memory while libstdc++ needs to be loaded anyway for the
# server process.
# Needs HAVE_WL_AS_NEEDED!


message("Shared compiler flags: ${ACNG_CXXFLAGS_COMMON}")
message("Shared linker flags: ${CMAKE_EXE_LINKER_FLAGS}")

SET(CMAKE_REQUIRED_LIBRARIES "")
SET(CMAKE_REQUIRED_FLAGS ${ACNG_CXXFLAGS_COMMON})

# I don't need -rdynamic, thanks!
SET(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")
SET(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")

#######################################
# all checks done, save configuration #
#######################################

CONFIGURE_FILE("${CMAKE_SOURCE_DIR}/include/acsyscap.h.in" "${CMAKE_BINARY_DIR}/acsyscap.h")

add_subdirectory(client)
add_subdirectory(fs)
add_subdirectory(tool)
add_subdirectory(source)

###
### Extra install rules for static files
###

install(DIRECTORY conf/ DESTINATION ${CFGDIR}
   FILES_MATCHING PATTERN "*.conf")
install(DIRECTORY conf/ DESTINATION ${LIBDIR}
   FILES_MATCHING REGEX "mirrors$|mirrors.gz$|html$|css$|default$")

if(NOT DEFINED(HTMLDIR))
		set(HTMLDIR ${DOCDIR}/html)
endif()
if(NOT DEFINED(MANDIR))
   set(HTMLDIR ${DATADIR}/man)
endif()
install(FILES doc/README doc/apt-cacher-ng.pdf DESTINATION ${DOCDIR})
install(DIRECTORY doc/html/ DESTINATION ${HTMLDIR}
   FILES_MATCHING PATTERN "*.*")
install(DIRECTORY doc/man/ DESTINATION ${MANDIR}/man8
   FILES_MATCHING PATTERN "*.8")
if(NOT DEFINED(AVAHIDIR))
   set(AVAHIDIR "${SYSCONFDIR}/avahi/services")
endif()
install(FILES contrib/apt-cacher-ng.service DESTINATION ${AVAHIDIR})

