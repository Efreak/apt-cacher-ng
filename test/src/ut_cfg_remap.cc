#include "gtest/gtest.h"
#include "dbman.h"
#include "csmapping.h"
#include "acfg.h"

#include <unordered_map>

TEST(cfg, remap)
{
	using namespace acng;
	/*
	ASSERT_TRUE(cfg::SetOption("Remap-foo = bla blub http://fakeserver/irgendwas ; http://realmirror/pub/kubuntu"));
	ASSERT_TRUE(cfg::SetOption("Remap-bar = bla blub http://fakeserver/irgendwas ; http://realmirror/pub/kubuntu"));
	ASSERT_TRUE(cfg::SetOption("Remap-foo: another_foo_prefix"));
	ASSERT_NO_THROW(cfg::PostProcConfig());
	*/
	cfg::tConfigBuilder builder(true, true);
	builder
	.AddOption("Remap-foo = bla blub http://fakeserver/irgendwas ; http://realmirror/pub/kubuntu")
	.AddOption("Remap-bar = bla blub http://fakeserver/irgendwas ; http://realmirror/pub/kubuntu");
	ASSERT_NO_THROW(builder.Build());

	ASSERT_NO_THROW(cfg::tConfigBuilder(false, false)
	.AddOption("Remap-foo = bla blub http://fakeserver/irgendwas ; http://realmirror/pub/kubuntu")
	.AddOption("Remap-bar = bla blub http://fakeserver/irgendwas ; http://realmirror/pub/kubuntu")
	.Build());
}

TEST(cfg, algos)
{
	ASSERT_NO_THROW(acng::check_algos());
}
