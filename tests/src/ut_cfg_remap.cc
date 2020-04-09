#include "gtest/gtest.h"
#include "dbman.h"
#include "csmapping.h"
#include "acfg.h"

#include "gmock/gmock.h"

#include <unordered_map>

class MockDb : public acng::IDbManager
{
public:
	MOCK_METHOD(void, MarkChangeVoluntaryCommit, (), (override));
	MOCK_METHOD(void, MarkChangeMandatoryCommit, (), (override));
	MOCK_METHOD(std::string, GetMappingSignature, (const std::string& name), (override));
	MOCK_METHOD(void, StoreMappingSignature, (const std::string& name, const std::string& sig), (override));
};

TEST(cfg, remap)
{
	using namespace acng;
	using namespace testing;
	//auto db = IDbManager::create();
	//std::unique_ptr<IDbManager> db;
	//db.reset(new MockDb);
	auto db = std::make_unique<MockDb>();
	EXPECT_CALL(*db, GetMappingSignature(Eq("foo"))).Times(1).WillRepeatedly(Return("invalid"));
	EXPECT_CALL(*db, GetMappingSignature(Eq("bar"))).Times(1).WillRepeatedly(Return("invalid"));
	// this will return with the checksum and try to update
	EXPECT_CALL(*db, StoreMappingSignature(Eq("foo"), Eq("a3.4c47d0bb80e03fc1999b1a842e816457b5"))).Times(1);
	EXPECT_CALL(*db, StoreMappingSignature(Eq("bar"), Eq("a3.4c03ee6d39c8f7898b913ff7f2d6a97c57"))).Times(1);
	cfg::tConfigBuilder builder(true, true);
	builder
	.AddOption("Remap-foo = bla blub http://fakeserver/irgendwas ; http://realmirror/pub/kubuntu")
	.AddOption("Remap-bar = bla blub http://another/somethjing ; http://realmirror/pub/kubuntu");
	ASSERT_NO_THROW(builder.Build(*db));

	db = std::make_unique<MockDb>();
	EXPECT_CALL(*db, GetMappingSignature(Eq("foo"))).Times(1).WillRepeatedly(Throw(SQLite::Exception()));
	cfg::tConfigBuilder builder2(false, false);
	builder2.AddOption("Remap-foo = bla blub http://fakeserver/irgendwas ; http://realmirror/pub/kubuntu")
		.AddOption("Remap-bar = bla blub http://fakeserver/irgendwas ; http://realmirror/pub/kubuntu");
	ASSERT_NO_THROW(builder2.Build(*db));
}
