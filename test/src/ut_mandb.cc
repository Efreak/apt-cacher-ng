#include "gtest/gtest.h"
#include "dbman.h"

#include "acfg.h"

#include <unordered_map>

TEST(dbman, startup)
{
	acng::cfg::cacheDirSlash = "/tmp/";
	std::cerr << CACHE_BASE + "_acng.sqlite3" << std::endl;
    acng::dbman *dbm;
    ASSERT_NO_THROW(dbm = new acng::dbman());
}

TEST(algorithms,bin_str_long_match)
{
	std::unordered_map<std::string,int> testPool = {
			{"abra/kadabra", 1},
			{"abba", 2},
			{"something/else", 3},
	};
	std::string result;
	int other_result;
	auto matcher = [&](const std::string& tstr)
		{
		auto it = testPool.find(tstr);
		if(it==testPool.end())
			return false;
		if(tstr.length() > result.length())
		{
			result = tstr;
			other_result = it->second;
		}
		return true;
		};
	acng::fish_longest_match(WITHLEN("something/else/matters/or/not"), '/', matcher);
	ASSERT_EQ(result, "something/else");
	result.clear();
	acng::fish_longest_match(WITHLEN("something/elsewhere/matters/or/not"), '/', matcher);
	ASSERT_EQ(result, "");
	result.clear();
	testPool.emplace("something", 42);
	acng::fish_longest_match(WITHLEN("something/elsewhere/matters/or/not"), '/', matcher);
	ASSERT_EQ(result, "something");
	result.clear();
	acng::fish_longest_match(WITHLEN(""), '/', matcher);
	EXPECT_TRUE(result.empty());
	acng::fish_longest_match(WITHLEN("abbakus"), '/', matcher);
	EXPECT_TRUE(result.empty());
	acng::fish_longest_match(WITHLEN("abba/forever.deb"), '/', matcher);
	EXPECT_EQ(result, "abba");
	result.clear();
	acng::fish_longest_match(WITHLEN("abra/kadabra/veryverylongletusseewhathappenswhenthestirng,lengthiswaytoomuchorwhat"), '/', matcher);
	EXPECT_EQ(result, "abra/kadabra");
	result.clear();
	acng::fish_longest_match(WITHLEN("veryverylongletusseewhathappenswhenthestirng,lengthiswaytoomuchorwhat"), '/', matcher);
	EXPECT_TRUE(result.empty());
	acng::fish_longest_match(WITHLEN("something/else"), '/', matcher);
	ASSERT_EQ(result, "something/else");

}
