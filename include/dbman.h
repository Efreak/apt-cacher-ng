/*
 * dbman.h
 *
 *  Created on: 23.02.2020
 *      Author: Eduard Bloch
 */

#ifndef INCLUDE_DBMAN_H_
#define INCLUDE_DBMAN_H_

#include <string>

namespace acng
{
class dbman
{
	struct dbdata;
	dbdata *m_data = nullptr;
public:

	~dbman();
	// this constructor might throw!
	dbman();

	void MarkChangeVoluntaryCommit();
	void MarkChangeMandatoryCommit();
};
}



#endif /* INCLUDE_DBMAN_H_ */
