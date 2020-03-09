#include "gtest/gtest.h"
#include "dbman.h"

#include "acfg.h"

#include <unordered_map>

TEST(cfg, remap)
{
	using namespace acng;
	ASSERT_TRUE(cfg::SetOption("Remap-foo = bla blub http://fakeserver/irgendwas ; http://realmirror/pub/kubuntu"));
	ASSERT_TRUE(cfg::SetOption("Remap-bar = bla blub http://fakeserver/irgendwas ; http://realmirror/pub/kubuntu"));
	ASSERT_TRUE(cfg::SetOption("Remap-foo: another_foo_prefix"));
	ASSERT_NO_THROW(cfg::PostProcConfig());
}
