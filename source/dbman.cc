/*
 * dbman.cc
 *
 *  Created on: 23.02.2020
 *      Author: Eduard Bloch
 */

#include "dbman.h"
#include "acfg.h"

#include <iostream>

using namespace std;

auto* CREAT_MAPPINGS = R"<<<(
CREATE TABLE "mappings" (
	"id"	INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE,
	"name"	TEXT NOT NULL,
	"active"	INTEGER NOT NULL DEFAULT 0,
	"signature"	TEXT NOT NULL
);
)<<<";

namespace acng
{

class DbManager : public IDbManager
{
	friend class TExplicitTransaction;
	int m_pending = -1;
	SQLite::Database db;
public:
	DbManager () :
			db(CACHE_BASE + "_acng.sqlite3", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE)
	{
		try
		{
			SQLite::Statement query(db, CREAT_MAPPINGS);
			query.exec();
		} catch (const SQLite::Exception &ex)
		{
			//std::cerr << ex.getErrorCode() << " - " << ex.getExtendedErrorCode()  << " ," << ex.getErrorStr() << "-" << ex.what() << std::endl;

			// 1 and 1 is ok, that is "logic error", "table already exists"
			if (ex.getErrorCode() != 1 || ex.getExtendedErrorCode() != 1)
				throw;
		}

#if 0
	    // Compile a SQL query, containing one parameter (index 1)
	    SQLite::Statement   query(m_data->db, "SELECT * FROM cfgfpr WHERE size > ?");

	    // Bind the integer value 6 to the first parameter of the SQL query
	    query.bind(1, 6);

	    // Loop to execute the query step by step, to get rows of result
	    while (query.executeStep())
	    {
	        // Demonstrate how to get some typed column value
	        int         id      = query.getColumn(0);
	        const char* value   = query.getColumn(1);
	        int         size    = query.getColumn(2);

	        std::cout << "row: " << id << ", " << value << ", " << size << std::endl;
	    }
#endif
	}
#if false
	void Sync() override
	{
	}
#endif

	std::string GetMappingSignature(const std::string &name) override
	{
#warning fixme
		return "foo";
	}

	void StoreMappingSignature(const std::string &name, const std::string &sig) override
	{
	}
#if false
	TExplicitTransaction GetExplicitTransaction() override
	{
		TExplicitTransaction ret;
		return std::move(ret);
	}
#endif
	void tetRollback()
	{
#warning fixme
	}
	void tetCommit()
	{

	}

};

std::unique_ptr<IDbManager> IDbManager::create()
{
	return std::make_unique<DbManager>();
}


void IDbManager::TExplicitTransaction::commit()
{
	if(!m_dbm) // XXX: user mistake? multiple commit?
		return;
	try {
		m_dbm->tetCommit();
		m_dbm=nullptr;
	}
	catch (...)
	{
		m_dbm = nullptr;
		throw;
	}
}

IDbManager::TExplicitTransaction::TExplicitTransaction(TExplicitTransaction &&src)
{
	swap(m_dbm, src.m_dbm);
}

IDbManager::TExplicitTransaction::~TExplicitTransaction()
{
	if(!m_dbm)
		return;
	m_dbm->tetRollback();
	m_dbm = nullptr;
}


}

