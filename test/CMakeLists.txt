
#include(GTest)
add_subdirectory(/usr/src/googletest gtest)

add_definitions(-DUNDER_TEST)
include_directories(src)

#file(GLOB_RECURSE TEST_SOURCES LIST_DIRECTORIES false src/*.h src/*.cc)
add_executable(ut_acng_misc
	src/ut_mandb.cc
	src/main.cc
	../source/dbman.cc
	../source/acfg_defaults.cc
	../source/meta.cc
	../source/acbuf.cc
	../source/errnofmt.cc
	)
target_link_libraries(ut_acng_misc gtest_main SQLiteCpp sqlite3)

add_executable(ut_cfg_remap
	src/ut_cfg_remap.cc
	src/main.cc
	../source/dbman.cc
	../source/acfg_defaults.cc
	../source/meta.cc
	../source/acbuf.cc
	../source/errnofmt.cc
	../source/acfg.cc
	../source/filereader.cc
	)
target_link_libraries(ut_cfg_remap gtest_main SQLiteCpp sqlite3 supacngTEST ${BaseNetworkLibs} ${ServerLibs} ${CompLibs} ${SSL_LIB_LIST} ${LDFLAGS_DAEMON} ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_LIBS_ACNG})

#target_compile_definitions(ut_cfg_remap PUBLIC -DNOCOMP -DMINIBUILD)
