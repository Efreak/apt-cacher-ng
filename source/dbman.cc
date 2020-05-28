/*
 * dbman.cc
 *
 *  Created on: 23.02.2020
 *      Author: Eduard Bloch
 */

#include "dbman.h"
#include "acfg.h"
#include "activity.h"
#include "fileio.h"
#include "acngbase.h"
#include <iostream>
#include <future>

#include <SQLiteCpp/Database.h>

using namespace std;

enum ECheckHow
{
	EXP_SUCCESS,
	EXP_FAILURE
};

struct tSetupStructure
{
	const char* name;
	const char* check;
	const char* create;
//	ECheckHow how;
//	int errcode, exterrcode;
};
/*
auto* CREAT_MAPPINGS = R"<<<(
CREATE TABLE "mappings" (
	"id"	INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE,
	"name"	TEXT NOT NULL,
	"active"	INTEGER NOT NULL DEFAULT 0,
	"signature"	TEXT NOT NULL
);
)<<<";
*/
const tSetupStructure setup_sequence[] =
{
		{
				"MAPPINGS",

				R"<<<(
select count(id), count(name), count(active), count(signature) from mappings;
				)<<<",

				R"<<<(
				CREATE TABLE "mappings" (
					"id"	INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT UNIQUE,
					"name"	TEXT NOT NULL,
					"active"	INTEGER NOT NULL DEFAULT 0,
					"signature"	TEXT NOT NULL
				);
				)<<<"
		}
};

namespace acng
{

class DbManager;
#if false
/**
 * Works analogue to SQLiteCpp::Transaction but correlate the timing with internal transactions.
 */
class TExplicitTransaction
{
	friend class DbManager;
	DbManager* m_dbm = nullptr;
	TExplicitTransaction() = default;
public:
	void commit();
	TExplicitTransaction(TExplicitTransaction&& src);
	~TExplicitTransaction();
};
#endif

volatile int nix;

class DbManager : public IDbManager
{
	friend class TExplicitTransaction;
	int m_pending = -1;
	std::unique_ptr<SQLite::Database> m_db;
	OpState m_opState = OpState::CLOSED;
	std::string m_sError;
	tSysRes& m_rc;

	void checkTables();
	void createTables();

public:
	DbManager (tSysRes& rc) : m_rc(rc) {};

	void runInThread(tCancelableAction ac)
	{

	}
	void Open(const std::string& path)
	{
		Cstat fstate(path);
		auto shouldBeGood = fstate && fstate.st_size > 0;

		if (shouldBeGood)
		{
			try
			{
				m_db.reset(new SQLite::Database(path, SQLite::OPEN_READWRITE));
			} catch (const SQLite::Exception &ex)
			{
				m_sError = string(
						"Error opening existing database. Reason: ") + ex.getErrorStr()
								+ ". Please check apt-cacher-ng manual page for recovery options.";
				throw tStartupException(m_sError);
			}
		}
		try
		{
			m_db.reset(
					new SQLite::Database(path,
							SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE));
		} catch (const SQLite::Exception &ex)
		{
			m_sError =
					string("Error creating the internal database. Reason: ") + ex.getErrorStr()
					+ ". Please check apt-cacher-ng manual page for recovery options.";
			throw tStartupException(m_sError);
		}

		for (const auto &instruction : setup_sequence)
		{
			try
			{
				SQLite::Statement query(*m_db, instruction.check);
				query.executeStep();
				continue;
			} catch (const SQLite::Exception&)
			{
			}
			try
			{
				SQLite::Statement query(*m_db, instruction.create);
				query.exec();
				continue;
			} catch (const SQLite::Exception& ex)
			{
				m_sError = string("Error creating essential table `")
						+ instruction.name + "` - " + ex.getErrorStr();
				throw tStartupException(m_sError);
			}
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

	std::string GetMappingSignature(const std::string &name) noexcept override
	{
#warning fixme
		return "foo";
	}

	void StoreMappingSignature(const std::string &name, const std::string &sig) noexcept override
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

void IDbManager::AddSqliteImpl(tSysRes& rc)
{
#warning this is crap, cancel will not report anything, what to do with cancel
	std::promise<void> flag;
	auto bgmake = [&](bool cncl){ if(!cncl) rc.db.reset(new DbManager(rc)); flag.set_value(); };
	rc.meta->Post(bgmake);
	return flag.get_future().get();
}

#if false
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

#endif
}

