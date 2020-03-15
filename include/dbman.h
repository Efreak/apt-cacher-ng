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

namespace acng
{
class dbman
{
	class tImpl;
	tImpl *m_pImpl;
public:

	~dbman();
	// this constructor might throw!
	dbman();

	static dbman& instance(); // kinda singleton

	void MarkChangeVoluntaryCommit();
	void MarkChangeMandatoryCommit();

	std::string GetMappingSignature(const std::string& name);
	void StoreMappingSignature(const std::string& name, const std::string& sig, bool insertNew);
};
}



#endif /* INCLUDE_DBMAN_H_ */
