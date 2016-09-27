PROJECT(ACNG CXX C)

# sources to strip from the globed list, to be added as needed
LIST(APPEND MAINSRCS
        "${CMAKE_CURRENT_SOURCE_DIR}/apt-cacher.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/acngtool.cc"
)
LIST(APPEND SHAREDSRCS
        "${CMAKE_CURRENT_SOURCE_DIR}/acbuf.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/acfg.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/dirwalk.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/dlcon.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/tcpconnect.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/header.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/acfg_defaults.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/meta.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/filereader.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/aclogger.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/fileitem.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/filelocks.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/caddrinfo.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/fileio.cc"
)

# remaining srcs, only for apt-cacher-ng
FILE(GLOB SRCS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
list(SORT SRCS)
list(REMOVE_ITEM SRCS ${MAINSRCS})
list(REMOVE_ITEM SRCS ${SHAREDSRCS})

ADD_LIBRARY(acngstuff OBJECT ${SHAREDSRCS})
SET_TARGET_PROPERTIES(acngstuff PROPERTIES COMPILE_FLAGS "${ACNG_CXXFLAGS_COMMON} ${lsd_CFLAGS}")

ADD_EXECUTABLE(apt-cacher-ng $<TARGET_OBJECTS:acngstuff> ${SRCS} apt-cacher.cc)
TARGET_LINK_LIBRARIES(apt-cacher-ng ${BaseNetworkLibs} ${ServerLibs} ${CompLibs} ${SSL_LIB_LIST} ${lsd_LDFLAGS} ${EXTRA_LIBS_ACNG})
# I'd like this method more but apparently LINK_FLAGS is always prepended, no way to append it
#SET_TARGET_PROPERTIES(apt-cacher-ng PROPERTIES LINK_FLAGS "${lsd_LDFLAGS}")
SET_TARGET_PROPERTIES(apt-cacher-ng PROPERTIES COMPILE_FLAGS "${ACNG_CXXFLAGS_COMMON} ${lsd_CFLAGS}")
INSTALL(TARGETS apt-cacher-ng DESTINATION ${SBINDIR})

ADD_EXECUTABLE(acngtool acngtool.cc $<TARGET_OBJECTS:acngstuff>)
SET_TARGET_PROPERTIES(acngtool PROPERTIES COMPILE_FLAGS "${ACNG_CXXFLAGS_COMMON}")
TARGET_LINK_LIBRARIES(acngtool ${BaseNetworkLibs} ${CompLibs} ${SSL_LIB_LIST} ${EXTRA_LIBS_ACNGTOOL})
INSTALL(TARGETS acngtool DESTINATION ${LIBDIR})

