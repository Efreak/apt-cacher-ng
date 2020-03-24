#include "gtest/gtest.h"
#include "dbman.h"

#include "acfg.h"

#include <unordered_map>

TEST(dbman, startup)
{
	acng::cfg::cacheDirSlash = "/tmp/";
	std::cerr << CACHE_BASE + "_acng.sqlite3" << std::endl;
    //acng::IDbManager *dbm;
    ASSERT_NO_THROW(acng::IDbManager::create());
}

