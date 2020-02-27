
#include(GTest)
add_subdirectory(/usr/src/googletest gtest)

file(GLOB_RECURSE TEST_SOURCES LIST_DIRECTORIES false src/*.h src/*.cc)
add_executable(ut_acng ${TEST_SOURCES}
	../source/dbman.cc
	)
target_link_libraries(ut_acng gtest_main)
