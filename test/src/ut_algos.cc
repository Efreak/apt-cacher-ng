#include "gtest/gtest.h"
#include "dbman.h"
#include "csmapping.h"
#include "acfg.h"

#include "gmock/gmock.h"

#include <unordered_map>

TEST(algorithms, checksumming)
{
	ASSERT_NO_THROW(acng::check_algos());

	acng::tChecksum cs, csmd(acng::CSTYPES::MD5);
	cs.Set("a26a96c0c63589b885126a50df83689cc719344f4eb4833246ed57f42cf67695a2a3cefef5283276cbf07bcb17ed7069de18a79410a5e4bc80a983f616bf7c91");

	auto cser2 = acng::csumBase::GetChecker(acng::CSTYPES::SHA512);
	cser2->add("bf1942");
	auto cs2=cser2->finish();
	ASSERT_EQ(cs2, cs);

	ASSERT_NE(csmd, cs2);
}

TEST(algorithms,bin_str_long_match)
{
	using namespace acng;
	std::map<string_view,int> testPool = {
			{"abra/kadabra", 1},
			{"abba", 2},
			{"something/else", 3},
	};
	auto best_result = testPool.end();
	auto probe_count=0;
	// the costly lookup operation we want to call as few as possible
	auto matcher = [&](string_view tstr) ->bool
		{
		probe_count++;
		auto it = testPool.find(tstr);
		if(it==testPool.end())
			return false;
		// pickup the longest seen result
		if(best_result == testPool.end() || tstr.length() > best_result->first.length())
			best_result=it;
		return true;
		};

	fish_longest_match("something/else/matters/or/not", '/', matcher);
	ASSERT_NE(testPool.end(), best_result);
	ASSERT_EQ(best_result->first, "something/else");
	best_result = testPool.end();
	// not to be found
	fish_longest_match("something/elsewhere/matters/or/not", '/', matcher);
	ASSERT_EQ(testPool.end(), best_result);

	testPool.emplace("something", 42);
	fish_longest_match("something/elsewhere/matters/or/not", '/', matcher);
	ASSERT_NE(testPool.end(), best_result);
	ASSERT_EQ(best_result->first, "something");
	best_result = testPool.end();

	fish_longest_match("", '/', matcher);
	ASSERT_EQ(testPool.end(), best_result);

	fish_longest_match("/", '/', matcher);
	ASSERT_EQ(testPool.end(), best_result);

	acng::fish_longest_match("abbakus", '/', matcher);
	ASSERT_EQ(testPool.end(), best_result);

	acng::fish_longest_match("abba/forever.deb", '/', matcher);
	ASSERT_NE(testPool.end(), best_result);
	EXPECT_EQ(best_result->first, "abba");
	best_result = testPool.end();

	acng::fish_longest_match("abra/kadabra/veryverylongletusseewhathappenswhenthestirng,lengthiswaytoomuchorwhat", '/', matcher);
	ASSERT_NE(testPool.end(), best_result);
	EXPECT_EQ(best_result->first, "abra/kadabra");
	best_result = testPool.end();

	acng::fish_longest_match("veryverylongletusseewhathappenswhenthestirng,lengthiswaytoomuchorwhat", '/', matcher);
	ASSERT_EQ(testPool.end(), best_result);
	acng::fish_longest_match("something/else", '/', matcher);
	ASSERT_NE(testPool.end(), best_result);
	ASSERT_EQ(best_result->first, "something/else");
	best_result = testPool.end();

	// the longest match
	fish_longest_match("abra/kadabra", '/', matcher);
	ASSERT_NE(testPool.end(), best_result);
	ASSERT_EQ(best_result->first, "abra/kadabra");
	best_result = testPool.end();

	probe_count=0;
	fish_longest_match("bla bla blub ganz viele teile wer hat an der uhr ge dreh t ist es wir klich schon so frueh", ' ', matcher);
	ASSERT_EQ(testPool.end(), best_result);
	ASSERT_LE(probe_count, 5);
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
	std::deque<string_view> exp {"foo", "bar", "blalba"};

	tSplitWalk tknzr("  foo bar blalba", SPACECHARS, false);
	std::deque<string_view> result;
	for(auto it:tknzr) result.emplace_back(it);
	ASSERT_EQ(result, exp);
//#error und jetzt fuer stricten splitter

	tknzr.reset("foo    bar blalba    ");
//	std::vector<string_view> result2(tknzr.begin(), tknzr.end());

	auto q = tknzr.to_deque();
	ASSERT_EQ(exp, q);

	ASSERT_EQ(result, q);

	tSplitWalk strct("a;bb;;c", ";", true);
	std::deque<string_view> soll {"a", "bb", "", "c"};
	ASSERT_EQ(soll, strct.to_deque());
	strct.reset(";a;bb;;c");
	q = strct.to_deque();
	ASSERT_NE(soll, q);
	ASSERT_EQ(q.front(), "");
	q.pop_front();
	ASSERT_EQ(soll, q);
	strct.reset(";a;bb;;c;");
	q = strct.to_deque();
	ASSERT_EQ(q.size(), 6);
	ASSERT_EQ(q.front(), "");
	ASSERT_EQ(q.back(), "");
}
