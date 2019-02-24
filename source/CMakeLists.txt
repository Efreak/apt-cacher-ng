set(SHAREDSRCS acbuf.cc acfg.cc acfg_defaults.cc aclogger.cc caddrinfo.cc dirwalk.cc dlcon.cc fileio.cc fileitem.cc filelocks.cc filereader.cc header.cc meta.cc tcpconnect.cc cleaner.cc job.cc bgtask.cc cacheman.cc conn.cc conserver.cc expiration.cc lockable.cc maintenance.cc mirror.cc pkgimport.cc rfc2553emu.cc showinfo.cc)
set(ACNG_SRCS apt-cacher.cc )
add_definitions(-DACNG_CORE_IN_SO)

ADD_LIBRARY(supacng SHARED ${SHAREDSRCS})
SET_TARGET_PROPERTIES(supacng PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${CFLAGS_DAEMON} ${CFLAGS_PTHREAD}")
TARGET_LINK_LIBRARIES(supacng ${BaseNetworkLibs} ${ServerLibs} ${CompLibs} ${SSL_LIB_LIST} ${LDFLAGS_DAEMON} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIBS_ACNG})
INSTALL(TARGETS supacng LIBRARY DESTINATION lib)

ADD_EXECUTABLE(apt-cacher-ng ${ACNG_SRCS} apt-cacher.cc $<TARGET_OBJECTS:osslcompat>)
TARGET_LINK_LIBRARIES(apt-cacher-ng supacng ${BaseNetworkLibs} ${ServerLibs} ${CompLibs} ${SSL_LIB_LIST} ${LDFLAGS_DAEMON} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIBS_ACNG})
SET_TARGET_PROPERTIES(apt-cacher-ng PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${CFLAGS_DAEMON} ${CFLAGS_PTHREAD}")
INSTALL(TARGETS apt-cacher-ng DESTINATION ${SBINDIR})

ADD_EXECUTABLE(acngtool acngtool.cc $<TARGET_OBJECTS:osslcompat>)
SET_TARGET_PROPERTIES(acngtool PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${CFLAGS_PTHREAD}")
TARGET_LINK_LIBRARIES(acngtool supacng ${BaseNetworkLibs} ${CompLibs} ${SSL_LIB_LIST} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIBS_ACNGTOOL})
INSTALL(TARGETS acngtool DESTINATION ${LIBDIR})

