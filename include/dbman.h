/*
 * dbman.h
 *
 *  Created on: 23.02.2020
 *      Author: Eduard Bloch
 */

#ifndef INCLUDE_DBMAN_H_
#define INCLUDE_DBMAN_H_

#include <SQLiteCpp/Database.h>
#include <string>
#include <memory>

namespace acng
{
class DbManager;

class IDbManager
{
public:

	virtual ~IDbManager() {};

	static std::unique_ptr<IDbManager> create();

	virtual std::string GetMappingSignature(const std::string& name) =0;
	/**
	 * Will delete old signature data and insert a new one
	 */
	virtual void StoreMappingSignature(const std::string& name, const std::string& sig) =0;

	/**
	 * Works analog to SQLiteCpp::Transaction but correlate the timing with internal transactions.
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

#if false
	/**
	 * Transfers the responsibility for a transaction execution to the user of the returned object.
	 * Data is flushed before, and might be reverted if the user does not commit this TA.
	 */
	virtual TExplicitTransaction GetExplicitTransaction() =0;
#endif
};

}



#endif /* INCLUDE_DBMAN_H_ */
