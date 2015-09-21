PROJECT(ACNG CXX C)

# sources to strip from the globed list, to be added as needed
LIST(APPEND OPTSOURCES  
	#"${CMAKE_SOURCE_DIR}/source/sha1.cc" "${CMAKE_SOURCE_DIR}/source/md5.cc" "${CMAKE_SOURCE_DIR}/source/sha2.cc" 
	"${CMAKE_SOURCE_DIR}/source/apt-cacher.cc")
# keep "${CMAKE_SOURCE_DIR}/source/rfc2553emu.cc" because auto-disables its implementation where unneeded

FILE(GLOB SRCS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
list(REMOVE_ITEM SRCS ${OPTSOURCES})

ADD_LIBRARY(acngstuff STATIC ${SRCS} ${NONOSSLSRCS})
SET_TARGET_PROPERTIES(acngstuff PROPERTIES COMPILE_FLAGS "${ACNG_CXXFLAGS_COMMON} ${lsd_CFLAGS}")

ADD_EXECUTABLE(apt-cacher-ng ${CMAKE_CURRENT_SOURCE_DIR}/apt-cacher.cc)
TARGET_LINK_LIBRARIES(apt-cacher-ng acngstuff ${ServerLibs} ${CompLibs} ${SSL_LIB_LIST} ${lsd_LDFLAGS} ${EXTRA_LIBS_ACNG})
# I'd like this method more but apparently LINK_FLAGS is always prepended, no way to append it
#SET_TARGET_PROPERTIES(apt-cacher-ng PROPERTIES LINK_FLAGS "${lsd_LDFLAGS}")
SET_TARGET_PROPERTIES(apt-cacher-ng PROPERTIES COMPILE_FLAGS "${ACNG_CXXFLAGS_COMMON} ${lsd_CFLAGS}")
INSTALL(TARGETS apt-cacher-ng DESTINATION ${SBINDIR})

