cmake_minimum_required(VERSION 2.6.2)
set(CMAKE_LEGACY_CYGWIN_WIN32 0)

PROJECT(ACNG CXX C)

INCLUDE(CheckIncludeFiles) 
INCLUDE(CheckCXXSourceCompiles)
INCLUDE(CheckCXXCompilerFlag)
INCLUDE(CheckTypeSize)
INCLUDE(TestBigEndian)
INCLUDE(CheckLibraryExists)

IF(NOT DEFINED(PREFIX))
set(PREFIX "/usr/local" CACHE STRING "Target installation space")
ENDIF()
IF(NOT DEFINED(CFGDIR))
	set(CFGDIR "/etc/apt-cacher-ng" CACHE STRING "Location of system-wide configuration files")
ENDIF()

# Various feature checks
CHECK_INCLUDE_FILES ("sys/param.h;sys/mount.h" HAVE_SYS_MOUNT_H)
CHECK_INCLUDE_FILES ("sys/vfs.h" HAVE_SYS_VFS_H)
CHECK_TYPE_SIZE(int SIZE_INT)
CHECK_TYPE_SIZE(long SIZE_LONG)
TEST_BIG_ENDIAN(WORDS_BIGENDIAN)

INCLUDE_DIRECTORIES(. ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR} "include")

FILE(GLOB SRCS "${CMAKE_SOURCE_DIR}/source/*.cc")

LIST(APPEND TOOLSRCS tool/main.cc source/acfg.cc source/meta.cc source/dlcon.cc
	source/tcpconnect.cc source/acfg_defaults.cc source/acbuf.cc
	source/filereader.cc source/fileitem.cc source/acbuf.cc source/aclogger.cc
	source/fileio.cc source/filelocks.cc source/header.cc source/caddrinfo.cc)


IF(NOT DEFINED(ACVERSION))
FILE(READ "${CMAKE_SOURCE_DIR}/VERSION" ACVERSION)
string(REPLACE "\n" "" ACVERSION ${ACVERSION})
MESSAGE(STATUS "Version: ${ACVERSION}")
ENDIF()

# minimum prerequisites
list(APPEND AcngLdList pthread)

# common required flags to be used later for CMAKE_REQUIRED_FLAGS and the build,
# CXXFLAGS (via initial CMAKE_CXX_FLAGS) extends and overrides them
SET(ACNG_CXXFLAGS_COMMON " -pthread -g -O2 -Wall -Wextra -Wno-unused-parameter -D_FILE_OFFSET_BITS=64 ${CMAKE_CXX_FLAGS}")
# same for linker flags, imported from LDFLAGS environment var
SET(LDFLAGS_MIN "${CMAKE_EXE_LINKER_FLAGS}")

message("Initial CXX flags: ${ACNG_CXXFLAGS_COMMON}")
message("Initial linker flags: ${LDFLAGS_MIN}")

IF(CMAKE_SYSTEM MATCHES "Darwin")
   SET(ACNG_CXXFLAGS_COMMON " ${ACNG_CXXFLAGS_COMMON} -D_DARWIN_C_SOURCE ")
ENDIF(CMAKE_SYSTEM MATCHES "Darwin")

IF(CMAKE_SYSTEM MATCHES "CYGWIN")
   SET(ACNG_CXXFLAGS_COMMON " ${ACNG_CXXFLAGS_COMMON}  -U__STRICT_ANSI__ -DNOMINMAX ")

option(USE_LTO "Enable Link Time Optimization (requires modern compilers)" off)
ELSE()
option(USE_LTO "Enable Link Time Optimization (requires modern compilers)" on)
ENDIF()

FILE(READ test/build/testcxx11.cc CXX11_TESTSRC)

# prefer that style if possible since since Cygwin seems to have an old bug with missing definitions, http://cygwin.com/ml/cygwin/2012-04/msg00140.html
CHECK_CXX_COMPILER_FLAG(-std=gnu++11 COMPILER_SUPPORTS_GNU11)
CHECK_CXX_COMPILER_FLAG(-std=c++11 COMPILER_SUPPORTS_CXX11)
CHECK_CXX_COMPILER_FLAG(-std=c++0x COMPILER_SUPPORTS_CXX0X)
if(COMPILER_SUPPORTS_GNU11)
   set(ACNG_CXXFLAGS_COMMON "${ACNG_CXXFLAGS_COMMON} -std=gnu++11")
elseif(COMPILER_SUPPORTS_CXX11)
   set(ACNG_CXXFLAGS_COMMON "${ACNG_CXXFLAGS_COMMON} -std=c++11")
elseif(COMPILER_SUPPORTS_CXX0X)
   set(ACNG_CXXFLAGS_COMMON "${ACNG_CXXFLAGS_COMMON} -std=c++0x")
else()
   SET(CMAKE_REQUIRED_FLAGS ${ACNG_CXXFLAGS_COMMON})
   CHECK_CXX_SOURCE_COMPILES("${CXX11_TESTSRC}" COMPILER_SUPPORTS_CXX11_DEFAULT)
   if(NOT COMPILER_SUPPORTS_CXX11_DEFAULT)
      message(FATAL_ERROR "!! Error: failed to configure compiler for C++11 support. For GCC, version 4.7 or newer is required.")
   endif()
endif()

if(USE_LTO)
SET(CMAKE_REQUIRED_FLAGS "${ACNG_CXXFLAGS_COMMON} -flto")
SET(CMAKE_EXE_LINKER_FLAGS "${LDFLAGS_MIN} -flto ")
CHECK_CXX_SOURCE_COMPILES("${CXX11_TESTSRC}" USE_LTO)
if(USE_LTO)
   SET(ACNG_CXXFLAGS_COMMON ${CMAKE_REQUIRED_FLAGS})
   SET(LDFLAGS_MIN ${CMAKE_EXE_LINKER_FLAGS})
else(USE_LTO)
   SET(CMAKE_EXE_LINKER_FLAGS "${LDFLAGS_MIN}")
   message(WARNING "Link Time Optimization support broken, disabling it.")
endif(USE_LTO)
endif()

SET(CMAKE_REQUIRED_FLAGS ${ACNG_CXXFLAGS_COMMON})
SET(CMAKE_EXE_LINKER_FLAGS "${LDFLAGS_MIN}  -Wl,--as-needed ")
CHECK_CXX_SOURCE_COMPILES("${CXX11_TESTSRC}" USE_WLASNEEDED)

CHECK_CXX_COMPILER_FLAG("-Wl,--as-needed" USE_WLASNEEDED)
if(USE_WLASNEEDED)
   SET(LDFLAGS_MIN "${LDFLAGS_MIN} -Wl,--as-needed")
endif()

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

SET(SSL_LIB_LIST ssl crypto)
SET(CMAKE_REQUIRED_LIBRARIES "${SSL_LIB_LIST}")
FILE(READ test/build/HAVE_SSL.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_SSL)
IF(HAVE_SSL)
   list(APPEND AcngLdList "${SSL_LIB_LIST}")
   LIST(REMOVE_ITEM SRCS "${CMAKE_SOURCE_DIR}/source/sha1.cc")
   LIST(REMOVE_ITEM SRCS "${CMAKE_SOURCE_DIR}/source/md5.cc")
ELSE(HAVE_SSL)
   MESSAGE("!! OpenSSL not found or not working, disabling support")
   LIST(APPEND TOOLSRCS "${CMAKE_SOURCE_DIR}/source/sha1.cc" "${CMAKE_SOURCE_DIR}/source/md5.cc")
   SET(HAVE_SSL )
   SET(SSL_LIB_LIST )
ENDIF(HAVE_SSL)
SET(CMAKE_REQUIRED_LIBRARIES "")

SET(CMAKE_REQUIRED_LIBRARIES wrap)
FILE(READ test/build/HAVE_LIBWRAP.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_LIBWRAP)
IF(HAVE_LIBWRAP)
	list(APPEND AcngLdList wrap)
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

FIND_LIBRARY(HAVE_SOCKETLIB socket) # separate socket lib looks like Solaris-like environment
if(HAVE_SOCKETLIB)
   LIST(APPEND NetworkLibs socket nsl)
endif(HAVE_SOCKETLIB)

INCLUDE(FindPkgConfig)

pkg_check_modules(lsd "libsystemd>=209")
if(NOT lsd_FOUND)
pkg_check_modules(lsd libsystemd-daemon)
endif()
string(REPLACE ";" " " lsd_LDFLAGS "${lsd_LDFLAGS}")
string(REPLACE ";" " " lsd_CFLAGS "${lsd_CFLAGS}")
set(HAVE_SD_NOTIFY ${lsd_FOUND})

FILE(READ test/build/HAVE_MEMORY_SPTR.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_MEMORY_SPTR)

FILE(READ test/build/HAVE_TR1_MEMORY.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_TR1_MEMORY)

SET(CMAKE_REQUIRED_INCLUDES . ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})
FILE(READ test/build/HAVE_BOOST_SMARTPTR.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_BOOST_SMARTPTR)
set(CMAKE_REQUIRED_INCLUDES "")

if(NOT HAVE_BOOST_SMARTPTR)
   if(NOT HAVE_TR1_MEMORY)
      if(NOT HAVE_MEMORY_SPTR)
         message(FATAL_ERROR "Could not find a working smart pointer implementation. Please read documentation and include boost headers.")
      endif(NOT HAVE_MEMORY_SPTR)
   endif(NOT HAVE_TR1_MEMORY)
endif(NOT HAVE_BOOST_SMARTPTR)

SET(CMAKE_REQUIRED_LIBRARIES dl)
FILE(READ test/build/HAVE_DLOPEN.cc TESTSRC)
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_DLOPEN)

##################################
# All checks done, start building

message("Common CXX flags: ${ACNG_CXXFLAGS_COMMON}")
message("Common linker flags: ${LDFLAGS_MIN}")

SET(CMAKE_REQUIRED_LIBRARIES "")
SET(CMAKE_REQUIRED_FLAGS ${ACNG_CXXFLAGS_COMMON})

# I don't need -rdynamic, thanks!
SET(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")
SET(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")

ADD_EXECUTABLE(apt-cacher-ng ${SRCS})
list(REMOVE_DUPLICATES AcngLdList)
TARGET_LINK_LIBRARIES(apt-cacher-ng ${AcngLdList}  ${CompLibs} ${NetworkLibs} ${lsd_LDFLAGS} ${EXTRA_LIBS_ACNG})
# I'd like this method more but apparently LINK_FLAGS is always prepended, no way to append it
#SET_TARGET_PROPERTIES(apt-cacher-ng PROPERTIES LINK_FLAGS "${lsd_LDFLAGS}")
SET_TARGET_PROPERTIES(apt-cacher-ng PROPERTIES COMPILE_FLAGS "${ACNG_CXXFLAGS_COMMON} ${lsd_CFLAGS}")

ADD_EXECUTABLE(in.acng client/client.cc)
SET_TARGET_PROPERTIES(in.acng PROPERTIES COMPILE_FLAGS "${ACNG_CXXFLAGS_COMMON} -DMINIBUILD")
TARGET_LINK_LIBRARIES(in.acng ${NetworkLibs} ${EXTRA_LIBS_INETD})

ADD_EXECUTABLE(acngtool ${TOOLSRCS})
SET_TARGET_PROPERTIES(acngtool PROPERTIES COMPILE_FLAGS "${ACNG_CXXFLAGS_COMMON}")
TARGET_LINK_LIBRARIES(acngtool ${SSL_LIB_LIST} ${CompLibs} ${EXTRA_LIBS_INETD})

# -DEXTRA_LIBS_INETD=-lsupc++
# funny hack, link with gcc and avoid libstdc++/libm (since no STL parts used
# there). However, it needs to be investigated - the alternative linking makes
# the binary ~40kb larger, might include higher relocation costs and bigger
# chunks of unique memory while libstdc++ needs to be loaded anyway for the
# server process.
# Needs HAVE_WL_AS_NEEDED!


############################################
## Attempt to build acngfs where possible ##
############################################

pkg_check_modules(fuse fuse)

string(REPLACE ";" " " acngfs_cflags "${ACNG_CXXFLAGS_COMMON};-DMINIBUILD;${fuse_CFLAGS}")

# double-check and make sure it compiles
FILE(READ test/build/HAVE_FUSE_25.cc TESTSRC)
SET(CMAKE_REQUIRED_FLAGS " ${ACNG_CXXFLAGS_COMMON} ${acngfs_cflags}")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_FUSE_25)

if(fuse_FOUND AND HAVE_FUSE_25)
   if(ADDDEBUGSRC)
      ADD_EXECUTABLE(acngfs fs/httpfs.cc source/lockable.cc source/header.cc source/caddrinfo.cc source/acbuf.cc source/acfg.cc source/acfg_defaults.cc source/tcpconnect.cc source/dlcon.cc source/fileitem.cc source/aclogger.cc source/meta.cc)
   else(ADDDEBUGSRC)
      ADD_EXECUTABLE(acngfs fs/httpfs.cc source/lockable.cc source/header.cc source/caddrinfo.cc source/acbuf.cc source/acfg.cc source/acfg_defaults.cc source/tcpconnect.cc source/dlcon.cc source/fileitem.cc source/meta.cc)
   endif(ADDDEBUGSRC)

   #message("uhm: ${fuse_CFLAGS} -- ${acngfs_cflags} -- ${fuse_LDFLAGS} -- ${HAVE_DLOPEN}")
   SET_TARGET_PROPERTIES(acngfs PROPERTIES COMPILE_FLAGS "${acngfs_cflags}" )
   if(HAVE_DLOPEN)
      TARGET_LINK_LIBRARIES(acngfs dl pthread  ${NetworkLibs} ${SSL_LIB_LIST} ${EXTRA_LIBS_ACNGFS})
   else(HAVE_DLOPEN)
      TARGET_LINK_LIBRARIES(acngfs ${fuse_LDFLAGS}  ${NetworkLibs} ${SSL_LIB_LIST} ${EXTRA_LIBS_ACNGFS})
   endif(HAVE_DLOPEN)

else(fuse_FOUND AND HAVE_FUSE_25)
   message("- FUSE not found or not compatible, not building acngfs")
endif(fuse_FOUND AND HAVE_FUSE_25)


#######################################
# all checks done, save configuration #
#######################################

CONFIGURE_FILE("${CMAKE_SOURCE_DIR}/include/acsyscap.h.in" "${CMAKE_BINARY_DIR}/acsyscap.h")
