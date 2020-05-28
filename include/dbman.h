/*
 * dbman.h
 *
 *  Created on: 23.02.2020
 *      Author: Eduard Bloch
 */

#ifndef INCLUDE_DBMAN_H_
#define INCLUDE_DBMAN_H_

#include <string>
#include <memory>

namespace acng
{
class DbManager;
class tSysRes;

class IDbManager
{
public:
	static void AddSqliteImpl(tSysRes&);
	enum class OpState
	{
		CLOSED,
		STARTING,
		ACTIVE,
		RECOVERY,
		ADMIN_NEEDED,
		SHUTDOWN
	};
#if false
	virtual OpState GetOpState() =0;
#endif
	virtual ~IDbManager() {};

	virtual void Open(const std::string& path) =0;

	/**
	 * @return Checksum if such mapping exists, empty string otherwise
	 */
	virtual std::string GetMappingSignature(const std::string& name) noexcept =0;
	/**
	 * Will delete old signature data and insert a new one
	 */
	virtual void StoreMappingSignature(const std::string& name, const std::string& sig) noexcept =0;

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
