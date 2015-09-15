PROJECT(ACNG CXX C)

LIST(APPEND TOOLSRCS main.cc ../source/acfg.cc ../source/meta.cc ../source/dlcon.cc
	../source/tcpconnect.cc ../source/acfg_defaults.cc ../source/acbuf.cc
	../source/filereader.cc ../source/fileitem.cc ../source/acbuf.cc ../source/aclogger.cc
	../source/fileio.cc ../source/filelocks.cc ../source/header.cc ../source/caddrinfo.cc)

ADD_EXECUTABLE(acngtool ${TOOLSRCS} ${NONOSSLSRCS})
SET_TARGET_PROPERTIES(acngtool PROPERTIES COMPILE_FLAGS "${ACNG_CXXFLAGS_COMMON}")
TARGET_LINK_LIBRARIES(acngtool ${BaseNetworkLibs} ${CompLibs} ${SSL_LIB_LIST} ${EXTRA_LIBS_ACNGTOOL})
INSTALL(TARGETS acngtool DESTINATION ${LIBDIR})

