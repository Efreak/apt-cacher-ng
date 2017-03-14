
############################################
## Attempt to build acngfs where possible ##
############################################

pkg_check_modules(fuse fuse)

_append(acngfs_cflags -DMINIBUILD ${fuse_CFLAGS} ${CFLAGS_PTHREAD})

# double-check and make sure it compiles
FILE(READ ../test/build/HAVE_FUSE_25.cc TESTSRC)
SET(CMAKE_REQUIRED_FLAGS "${ACNG_COMPFLAGS} ${acngfs_cflags}")
CHECK_CXX_SOURCE_COMPILES("${TESTSRC}" HAVE_FUSE_25)

if(fuse_FOUND AND HAVE_FUSE_25)
        # not linking acngstuff lib, too many dependencies, so building a custom variant with MINIBUILD flag
        list(APPEND fsSRCS httpfs.cc ../source/lockable.cc ../source/header.cc ../source/caddrinfo.cc ../source/acbuf.cc ../source/acfg.cc ../source/acfg_defaults.cc ../source/tcpconnect.cc ../source/dlcon.cc ../source/fileitem.cc ../source/meta.cc ../source/fileio.cc)

        if(CMAKE_BUILD_TYPE MATCHES Debug)
                list(APPEND fsSRCS ../source/aclogger.cc)
        endif()

        ADD_EXECUTABLE(acngfs ${fsSRCS} $<TARGET_OBJECTS:osslcompat>)
        SET_TARGET_PROPERTIES(acngfs PROPERTIES COMPILE_FLAGS "${ACNG_COMPFLAGS} ${ACNG_CXXFLAGS} ${acngfs_cflags}")
        INSTALL(TARGETS acngfs DESTINATION ${LIBDIR})
        if(HAVE_DLOPEN)
                _append(EXTRA_LIBS_ACNGFS dl)
        endif()
        TARGET_LINK_LIBRARIES(acngfs ${fuse_LDFLAGS} ${BaseNetworkLibs} ${SSL_LIB_LIST} ${EXTRA_LIBS_ACNGFS} ${CMAKE_THREAD_LIBS_INIT})

else(fuse_FOUND AND HAVE_FUSE_25)
        message("- FUSE not found or not compatible, not building acngfs")
endif(fuse_FOUND AND HAVE_FUSE_25)


