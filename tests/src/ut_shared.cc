#include "gtest/gtest.h"
#include "dbman.h"
#include "csmapping.h"
#include "acfg.h"
#include "acngbase.h"
#include "sockio.h"
#include "shareditempool.h"

#include "gmock/gmock.h"

using namespace acng;

// bad idea, needs to consider move operations
//unsigned g_smoketest=0;

TEST(sharedpool, get)
{
	struct testBuild
	{
		std::string nix;
		testBuild() {
		//	g_smoketest++;
		}
		~testBuild() {
		//	g_smoketest--;
		}
	};
	auto pool = make_lptr<TSharedItemPool<std::string, testBuild>>();
	bool was_new = false;
	auto elref = pool->Get("foo", was_new);
	EXPECT_EQ(pool->index.size(), 1);
	EXPECT_TRUE(was_new);
	auto elref2 = pool->Get("foo", was_new);
	EXPECT_FALSE(was_new);
	EXPECT_EQ(pool->index.size(), 1);
	ASSERT_EQ( elref.value().nix.c_str(), elref2.value().nix.c_str());
	elref.reset();
	EXPECT_EQ(pool->index.size(), 1);
	elref.reset();
	EXPECT_EQ(pool->index.size(), 1);
	elref2.reset();
	EXPECT_EQ(pool->index.size(), 0);
}
