#include "dbman.h"
#include "csmapping.h"
#include "acfg.h"
#include "acngbase.h"
#include "sockio.h"
#include "shareditempool.h"

//#include "gmock/gmock.h"
#include "gtest/gtest.h"

using namespace acng;
using namespace testing;

#if 1

// bad idea, needs to consider move operations
//unsigned g_smoketest=0;

TEST(sharedpool, simple)
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

TEST(timersharedpool, notimer)
{

}


struct TSample : public acng::ISharedResource, public tLintRefcounted
{
	size_t m_xyCount =0;
	size_t prev=0;
	void __gotUsed(size_t *causingCounter) override
	{
		/* XXX: good enough for now, add mock sequence later
		cout << "used? " << *causingCounter
				<< " by " << hex << causingCounter
				<<  " vs. " << &m_xyCount
				<< endl;
				*/
		ASSERT_EQ(causingCounter,&m_xyCount);
		ASSERT_TRUE(m_xyCount != prev && (m_xyCount == 0 || m_xyCount==1));
		prev = m_xyCount;
	}
};
using TXYReference = TReference<TSample, &TSample::m_xyCount>;

TEST(tref, simple)
{
	auto rsrc = acng::make_lptr<TSample>();
	{
		TXYReference TUserXy(rsrc);
		ASSERT_EQ(1, rsrc->m_xyCount);
		{
			TXYReference TUserXy2(rsrc);
			ASSERT_EQ(2, rsrc->m_xyCount);
			TXYReference TUserXy3(TUserXy);
			ASSERT_EQ(3, rsrc->m_xyCount);
			ASSERT_EQ(rsrc.get(), TUserXy.ptr.get());
		}
		ASSERT_EQ(1, rsrc->m_xyCount);
		{
			auto testCopy = TUserXy;
			ASSERT_EQ(2, rsrc->m_xyCount);
		}
	}
	ASSERT_EQ(0, rsrc->m_xyCount);
	TXYReference TUserXy(rsrc);
	ASSERT_EQ(1, rsrc->m_xyCount);
}

#endif
