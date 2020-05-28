#include "gtest/gtest.h"
#include "dbman.h"
#include "acngbase.h"
#include "acfg.h"

#include <unordered_map>

using namespace acng;

std::string path;

TEST(dbman, startup)
{
	cfg::cacheDirSlash = "/tmp/";
	auto tmpdir=getenv("TMPDIR");
	path = ((tmpdir && *tmpdir) ? tmpdir : "/tmp");
	path = path + '/' + std::to_string(random()) + "_UT.sqlite3";
	std::cerr << path << std::endl;
	tSysRes rc;
	rc.meta = MakeSelfThreadedActivity();
	ASSERT_NO_THROW(IDbManager::AddSqliteImpl(rc));
	ASSERT_TRUE(rc.db);
	ASSERT_NO_THROW(rc.db->Open(path));
//	rc.meta->StartShutdown();
}
