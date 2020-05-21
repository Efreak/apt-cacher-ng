set(SHAREDSRCS
dns/caddrinfo.cc
        sockio.cc acbuf.cc acfg.cc acfg_defaults.cc aclogger.cc dirwalk.cc fileio.cc  filelocks.cc 
filereader.cc header.cc meta.cc tcpconnect.cc  rex.cc astrop.cc dbman.cc errnofmt.cc csmapping.cc
)
#  dlcon.cc fileitem.cc cleaner.cc job.cc bgtask.cc cacheman.cc
# conn.cc conserver.cc expiration.cc lockable.cc maintenance.cc mirror.cc pkgimport.cc showinfo.cc evabase.cc dnsiter.cc tcpconfactory.cc tlsio.cc fileitemws.cc

set(ACNG_SRCS apt-cacher.cc)
add_definitions(-DACNG_CORE_IN_SO)
include_directories(dns)

ADD_LIBRARY(supacng SHARED ${SHAREDSRCS})
SET_TARGET_PROPERTIES(supacng PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${CFLAGS_DAEMON} ${CFLAGS_PTHREAD}")
TARGET_LINK_LIBRARIES(supacng ${BaseNetworkLibs} ${ServerLibs} ${CompLibs} ${SSL_LIB_LIST} ${LDFLAGS_DAEMON} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIBS_ACNG})
INSTALL(TARGETS supacng LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

ADD_EXECUTABLE(apt-cacher-ng ${ACNG_SRCS} apt-cacher.cc)
TARGET_LINK_LIBRARIES(apt-cacher-ng supacng ${BaseNetworkLibs} ${ServerLibs} ${CompLibs} ${SSL_LIB_LIST} ${LDFLAGS_DAEMON} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIBS_ACNG})
SET_TARGET_PROPERTIES(apt-cacher-ng PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${CFLAGS_DAEMON} ${CFLAGS_PTHREAD}")
INSTALL(TARGETS apt-cacher-ng DESTINATION ${CMAKE_INSTALL_SBINDIR})

ADD_EXECUTABLE(acngtool acngtool.cc)
SET_TARGET_PROPERTIES(acngtool PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${CFLAGS_PTHREAD}")
TARGET_LINK_LIBRARIES(acngtool supacng ${BaseNetworkLibs} ${CompLibs} ${SSL_LIB_LIST} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIBS_ACNGTOOL})
INSTALL(TARGETS acngtool DESTINATION ${LIBDIR})

# build a special version for test with full symbol visibility
if(ENABLE_TESTS)
	ADD_LIBRARY(supacngTEST STATIC ${SHAREDSRCS})
  SET_TARGET_PROPERTIES(supacng PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${CFLAGS_DAEMON} ${CFLAGS_PTHREAD} -fvisibility=default -DUNDER_TEST")
endif()

