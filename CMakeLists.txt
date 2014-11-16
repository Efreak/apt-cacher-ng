cmake_minimum_required(VERSION 2.8.6)
set(CMAKE_LEGACY_CYGWIN_WIN32 0)

PROJECT(ACNG CXX C)

INCLUDE(CheckIncludeFiles) 
INCLUDE(CheckCXXSourceCompiles)
INCLUDE(CheckCXXSourceRuns)
INCLUDE(CheckCXXCompilerFlag)
INCLUDE(CheckTypeSize)
INCLUDE(TestBigEndian)
#INCLUDE (CheckLibraryExists)

# Various feature checks
CHECK_INCLUDE_FILES ("sys/param.h;sys/mount.h" HAVE_SYS_MOUNT_H)
CHECK_INCLUDE_FILES ("sys/vfs.h" HAVE_SYS_VFS_H)
CHECK_TYPE_SIZE(int SIZE_INT)
CHECK_TYPE_SIZE(long SIZE_LONG)
TEST_BIG_ENDIAN(WORDS_BIGENDIAN)

INCLUDE_DIRECTORIES(. ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR} "include")

FILE(GLOB SRCS "${CMAKE_SOURCE_DIR}/source/*.cc")

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
   SET(ACNG_CXXFLAGS_COMMON " ${ACNG_CXXFLAGS_COMMON}  -U__STRICT_ANSI__  ")
ENDIF()

# prefer that style if possible since since Cygin seems to have an old bug with missing definitions, http://cygwin.com/ml/cygwin/2012-04/msg00140.html
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
   message(FATAL_ERROR "!! Error: failed to configure the compiler with C++11 support. For GCC, version 4.7 or newer is required, a recent clang++ is also suitable.")
endif()

SET(CXX11_TESTSRC "struct z{int a=3;}; int main(){int a[3];for(auto x:a) return x;}")
if(NOLTO)
	message("++ Link Time Optimization disabled by user")
else(NOLTO)
   CHECK_CXX_COMPILER_FLAG(-flto USE_LTO)
   if(USE_LTO)
      SET(ACNG_CXXFLAGS_COMMON "${ACNG_CXXFLAGS_COMMON} -flto")
      SET(LDFLAGS_MIN "${LDFLAGS_MIN} -flto")
   endif()
endif(NOLTO)

CHECK_CXX_COMPILER_FLAG("-Wl,--as-needed" USE_WLASNEEDED)
if(USE_WLASNEEDED)
   SET(LDFLAGS_MIN "${LDFLAGS_MIN} -Wl,--as-needed")
endif()

#INCLUDE(FindZLIB) # broken, hangs for 10 seconds
# header check should be enough, gzip should be everywhere nowadays
#CHECK_INCLUDE_FILES("gzip.h" HAVE_ZLIB)
FIND_PATH(HAVE_ZLIB zlib.h )
if(HAVE_ZLIB)
	list(APPEND AcngLdList z)
	INCLUDE_DIRECTORIES(${HAVE_ZLIB})
else(HAVE_ZLIB)
   message(FATAL_ERROR "!! apt-cacher-ng requires gzip library and development files ${HAVE_ZLIB}")
endif(HAVE_ZLIB)

INCLUDE(FindBZip2)
if (BZIP2_FOUND)
   SET(HAVE_LIBBZ2 1)
   MARK_AS_ADVANCED(HAVE_LIBBZ2)
	INCLUDE_DIRECTORIES(${BZIP2_INCLUDE_DIR})
	list(APPEND AcngLdList bz2)
else (BZIP2_FOUND)
   message("!! apt-cacher-ng requires bzip2 library and development files for bz2 format support")
endif (BZIP2_FOUND)

SET(CMAKE_REQUIRED_FLAGS ${ACNG_CXXFLAGS_COMMON})
SET(CMAKE_REQUIRED_LIBRARIES lzma)
SET(TESTSRC "
#include <lzma.h>
lzma_stream t; int main(){ return lzma_stream_decoder (&t, 32000000, LZMA_TELL_UNSUPPORTED_CHECK | LZMA_CONCATENATED); }
")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_LZMA)
IF(HAVE_LZMA)
   list(APPEND AcngLdList lzma)
ELSE(HAVE_LZMA)
   MESSAGE("!! XZ (liblzma) not found or not working, disabling support")
   SET(HAVE_LZMA )
ENDIF(HAVE_LZMA)
SET(CMAKE_REQUIRED_LIBRARIES "")

SET(SSL_LIB_LIST ssl crypto)
SET(CMAKE_REQUIRED_LIBRARIES "${SSL_LIB_LIST}")
SET(TESTSRC "#include \"${CMAKE_SOURCE_DIR}/include/testssl.h\"")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_SSL)
IF(HAVE_SSL)
   list(APPEND AcngLdList "${SSL_LIB_LIST}")
   LIST(REMOVE_ITEM SRCS "${CMAKE_SOURCE_DIR}/source/sha1.cc")
   LIST(REMOVE_ITEM SRCS "${CMAKE_SOURCE_DIR}/source/md5.cc")
ELSE(HAVE_SSL)
   MESSAGE("!! OpenSSL not found or not working, disabling support")
   SET(HAVE_SSL )
   SET(SSL_LIB_LIST )
ENDIF(HAVE_SSL)
SET(CMAKE_REQUIRED_LIBRARIES "")

SET(CMAKE_REQUIRED_LIBRARIES wrap)
SET(TESTSRC "
#include <tcpd.h>
int main() { request_info req; request_init(&req, RQ_FILE, 0, 0); fromhost(&req); return !hosts_access(&req); }
")
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
set(HAVE_WORDEXP FALSE)
else()
SET(TESTSRC "
#include <wordexp.h>
int main(int argc, char **argv) { wordexp_t p; return wordexp(*argv, &p, 0); }
")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_WORDEXP)
endif()

SET(TESTSRC "
#include <glob.h>
int main(int argc, char **argv) { glob_t p; return glob(*argv, 0, 0, &p); }
")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_GLOB)

SET(TESTSRC "
#define _XOPEN_SOURCE 600
#include <fcntl.h>
int testme(int fd, off_t offset, off_t len, int) { return posix_fadvise(fd, offset, len, POSIX_FADV_SEQUENTIAL); }; int main(int,char**){return testme(0,0,0,0);}
")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_FADVISE)
# Solaris fallback
IF(NOT HAVE_FADVISE)
SET(TESTSRC "
#include <fcntl.h>
int testme(int fd, off_t offset, off_t len, int) { return posix_fadvise(fd, offset, len, POSIX_FADV_SEQUENTIAL); }; int main(int,char**){return testme(0,0,0,0);}
")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_FADVISE)
ENDIF(NOT HAVE_FADVISE)

SET(TESTSRC "
#include <sys/mman.h>
int testme(void *addr, size_t length, int advice) { return posix_madvise(addr, length, advice); } int main(int,char**){return testme(0,0,0);}
")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_MADVISE)

SET(TESTSRC "
#define _GNU_SOURCE
#include <linux/falloc.h>
#include <fcntl.h>
int main() { int fd=1; return fallocate(fd, FALLOC_FL_KEEP_SIZE, 1, 2); }
")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_LINUX_FALLOCATE)

SET(TESTSRC "
#include <sys/sendfile.h>
int main(int argc, char **argv) { off_t yes(3); return (int) sendfile(1, 2, &yes, 4); }
")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_LINUX_SENDFILE)

if("${CMAKE_SYSTEM_NAME}" MATCHES "Linux")
CHECK_CXX_SOURCE_COMPILES("#include \"${CMAKE_SOURCE_DIR}/include/testeventfd.h\"" HAVE_LINUX_EVENTFD)
endif()

SET(TESTSRC "
#include <unistd.h>
int main() { return pread(0, NULL, 0, 0); }
")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_PREAD)

SET(TESTSRC "
#include <unistd.h>
int main() { return daemon(0, 0); }
")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_DAEMON)

SET(TESTSRC "
#define _GNU_SOURCE
#include <fcntl.h>
int main() { loff_t sin(12), sout(34); return splice(0, &sin, 1, &sout, 12, SPLICE_F_MORE); }
")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_LINUX_SPLICE)

FIND_LIBRARY(HAVE_SOCKETLIB socket) # separate socket lib looks like Solaris-like environment
if(HAVE_SOCKETLIB)
   link_libraries(socket nsl)
endif(HAVE_SOCKETLIB)

INCLUDE(FindPkgConfig)

pkg_check_modules(lsd libsystemd-daemon)
string(REPLACE ";" " " lsd_LDFLAGS "${lsd_LDFLAGS}")
string(REPLACE ";" " " lsd_CFLAGS "${lsd_CFLAGS}")
set(HAVE_SD_NOTIFY ${lsd_FOUND})
#message("sd? ${HAVE_SD_NOTIFY} ${lsd_LDFLAGS}")

SET(TESTSRC "
#include <memory>
int main() { return NULL != std::shared_ptr<int>(new int(1)); }
")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_MEMORY_SPTR)

SET(TESTSRC "
#include <tr1/memory>
int main() { return NULL != std::tr1::shared_ptr<int>(new int(1)); }
")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_TR1_MEMORY)

set(CMAKE_REQUIRED_INCLUDES . ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})

SET(TESTSRC "
#include <boost/smart_ptr.hpp>
int main() { return NULL != boost::shared_ptr<int>(new int(1)); }
")
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
CHECK_CXX_SOURCE_COMPILES("#include \"${CMAKE_SOURCE_DIR}/include/testdlopen.h\"" HAVE_DLOPEN)

# maybe enable workaround for platforms with a weird implementation. XXX: this is not cross-compilation friendly, find a better solution
SET(CMAKE_REQUIRED_LIBRARIES pthread)
CHECK_CXX_SOURCE_RUNS("#include \"${CMAKE_SOURCE_DIR}/include/testpthread.h\"" PTHREAD_COND_TIMEDWAIT_TIME_RANGE_OK)

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
TARGET_LINK_LIBRARIES(apt-cacher-ng ${AcngLdList}  "${lsd_LDFLAGS}")
# I'd like this method more but apparently LINK_FLAGS is always prepended, no way to append it
#SET_TARGET_PROPERTIES(apt-cacher-ng PROPERTIES LINK_FLAGS "${lsd_LDFLAGS}")
SET_TARGET_PROPERTIES(apt-cacher-ng PROPERTIES COMPILE_FLAGS "${ACNG_CXXFLAGS_COMMON} ${lsd_CFLAGS}")

ADD_EXECUTABLE(in.acng client/client.cc)
SET_TARGET_PROPERTIES(in.acng PROPERTIES COMPILE_FLAGS ${ACNG_CXXFLAGS_COMMON})

#IF(HAVE_WL_AS_NEEDED)
# funny hack, link with gcc and avoid libstdc++/libm (since no STL parts used
# there). However, it needs to be investigated - the alternative linking makes
# the binary 40kb larger, might include higher relocation costs and bigger
# chunks of unique memory while libstdc++ needs to be loaded anyway for the
# server process.
#TARGET_LINK_LIBRARIES(in.acng supc++)
#SET_TARGET_PROPERTIES(in.acng PROPERTIES LINKER_LANGUAGE C)
#ENDIF(HAVE_WL_AS_NEEDED)


############################################
## Attempt to build acngfs where possible ##
############################################

pkg_check_modules(fuse fuse)

string(REPLACE ";" " " acngfs_cflags "${ACNG_CXXFLAGS_COMMON};-DMINIBUILD;${fuse_CFLAGS}")

# double-check and make sure it compiles
SET(TESTSRC "
#define _FILE_OFFSET_BITS 64
#define FUSE_USE_VERSION 25
#include <fuse.h>
int main() { return 0; }
")
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
      TARGET_LINK_LIBRARIES(acngfs dl pthread ${SSL_LIB_LIST})
   else(HAVE_DLOPEN)
      TARGET_LINK_LIBRARIES(acngfs ${fuse_LDFLAGS} ${SSL_LIB_LIST})
   endif(HAVE_DLOPEN)

else(fuse_FOUND AND HAVE_FUSE_25)
   message("- FUSE not found or not compatible, not building acngfs")
endif(fuse_FOUND AND HAVE_FUSE_25)


#######################################
# all checks done, save configuration #
#######################################

CONFIGURE_FILE("${CMAKE_SOURCE_DIR}/include/acsyscap.h.in" "${CMAKE_BINARY_DIR}/acsyscap.h")
