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

TEST(strop,views)
{
	using namespace acng;
	using namespace nonstd::string_view_literals;
	string_view a = "foo  ", b = "  foo";
	auto x=a;
	trimFront(x);
	ASSERT_EQ(x, "foo  ");
	x=a;
	trimBack(x);
	ASSERT_EQ(x, "foo");
	auto prex=b;
	trimFront(prex);
	ASSERT_EQ(prex, "foo");
	prex=b;
	trimBack(prex);
	ASSERT_EQ(prex, "  foo");

	string_view xtra("  ");
	trimFront(xtra);
	ASSERT_TRUE(xtra.empty());
	ASSERT_TRUE(xtra.data());
	ASSERT_FALSE(* xtra.data());
	xtra = "  ";
	// compiler shall use the same memory here
	ASSERT_EQ((void*) xtra.data(), (void*) "  ");
	trimBack(xtra);
	ASSERT_EQ((void*) xtra.data(), (void*) "  ");
	ASSERT_TRUE(xtra.empty());

	ASSERT_EQ("foo/bar", PathCombine(string_view(WITHLEN("foo")), string_view(WITHLEN("bar"))));
	ASSERT_EQ("foo/bar", PathCombine("foo", "/bar"));
	ASSERT_EQ("foo/bar", PathCombine(string_view(WITHLEN("foo/")), string_view(WITHLEN("/bar"))));
	ASSERT_EQ("foo/bar", PathCombine(string_view(WITHLEN("foo/")), string_view(WITHLEN("bar"))));

	tHttpUrl url;
	ASSERT_TRUE(url.SetHttpUrl("http://meh:1234/path/more/path?fragment"));
}

TEST(strop,splitter)
{
	using namespace acng;
	std::vector<string_view> exp {"foo", "bar", "blalba"};

	tSplitWalk tknzr("  foo bar blalba", SPACECHARS, false);
	std::vector<string_view> result;
	for(auto it:tknzr) result.emplace_back(it);
	ASSERT_EQ(result, exp);
//#error und jetzt fuer stricten splitter

	tknzr.reset("foo    bar blalba    ");
//	std::vector<string_view> result2(tknzr.begin(), tknzr.end());
	ASSERT_EQ(exp, std::vector<string_view>(tknzr.begin(), tknzr.end()));

}
