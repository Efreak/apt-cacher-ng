/*
 * dbman.cc
 *
 *  Created on: 23.02.2020
 *      Author: Eduard Bloch
 */

#include "dbman.h"
#include "acfg.h"
#include <SQLiteCpp/Database.h>

#include <iostream>

#define CREATE_FPRS "CREATE TABLE \"cfgfpr\" ( \"name\"	TEXT NOT NULL, \"purpose\" INTEGER NOT NULL, \"fpr\" BLOB NOT NULL);"

struct acng::dbman::dbdata
{
	int m_pending = -1;
	SQLite::Database db;
	dbdata() : db(CACHE_BASE + "_acng.sqlite3", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE)
	{
	}
};

acng::dbman::~dbman()
{
	delete m_data;
}

acng::dbman::dbman(): m_data(new dbdata)
{
    try
    {
        SQLite::Statement query(m_data->db, CREATE_FPRS);
    	query.exec();
    }
    catch(const SQLite::Exception& ex)
    {
    	//std::cerr << ex.getErrorCode() << " - " << ex.getExtendedErrorCode()  << " ," << ex.getErrorStr() << "-" << ex.what() << std::endl;

    	// 1 and 1 is ok, that is "logic error", "table already exists"
    	if(ex.getErrorCode() != 1 || ex.getExtendedErrorCode() != 1)
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

void acng::dbman::MarkChangeVoluntaryCommit()
{
}

void acng::dbman::MarkChangeMandatoryCommit()
{
}
