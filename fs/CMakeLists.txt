PROJECT(ACNG CXX C)


############################################
## Attempt to build acngfs where possible ##
############################################

pkg_check_modules(fuse fuse)

string(REPLACE ";" " " acngfs_cflags "${ACNG_CXXFLAGS_COMMON};-DMINIBUILD;${fuse_CFLAGS}")

# double-check and make sure it compiles
FILE(READ ../test/build/HAVE_FUSE_25.cc TESTSRC)
SET(CMAKE_REQUIRED_FLAGS "${ACNG_CXXFLAGS_COMMON} ${acngfs_cflags}")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_FUSE_25)

if(fuse_FOUND AND HAVE_FUSE_25)
        # not linking acngstuff lib, too many dependencies
	list(APPEND fsSRCS httpfs.cc ../source/lockable.cc ../source/header.cc ../source/caddrinfo.cc ../source/acbuf.cc ../source/acfg.cc ../source/acfg_defaults.cc ../source/tcpconnect.cc ../source/dlcon.cc ../source/fileitem.cc ../source/meta.cc ../source/fileio.cc)

	if(CMAKE_BUILD_TYPE MATCHES Debug)
		list(APPEND fsSRCS ../source/aclogger.cc)
	endif()

	ADD_EXECUTABLE(acngfs ${fsSRCS} ${sslworkaroundStuff})
	INSTALL(TARGETS acngfs DESTINATION ${LIBDIR})

	#message("uhm: ${fuse_CFLAGS} -- ${acngfs_cflags} -- ${fuse_LDFLAGS} -- ${HAVE_DLOPEN}")
	SET_TARGET_PROPERTIES(acngfs PROPERTIES COMPILE_FLAGS "${acngfs_cflags}" )
	if(HAVE_DLOPEN)
		TARGET_LINK_LIBRARIES(acngfs dl pthread ${BaseNetworkLibs} ${SSL_LIB_LIST} ${EXTRA_LIBS_ACNGFS})
	else(HAVE_DLOPEN)
		TARGET_LINK_LIBRARIES(acngfs ${fuse_LDFLAGS} ${BaseNetworkLibs} ${SSL_LIB_LIST} ${EXTRA_LIBS_ACNGFS})
	endif(HAVE_DLOPEN)

else(fuse_FOUND AND HAVE_FUSE_25)
	message("- FUSE not found or not compatible, not building acngfs")
endif(fuse_FOUND AND HAVE_FUSE_25)


