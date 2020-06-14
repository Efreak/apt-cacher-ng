#include "gtest/gtest.h"
#include <vector>
#include <string>

std::vector<std::string> g_args;

int main(int argc, char **argv) {
	g_args.assign(argv, argv+argc);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
