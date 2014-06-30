/*
 * namedmutex.h
 *
 *  Created on: 29.06.2014
 *      Author: ed
 */

#ifndef NAMEDMUTEX_H_
#define NAMEDMUTEX_H_

#include <map>

#include "lockable.h"

// a quick-and-dirty class which makes heavy use of reference copies
// and therefore must only be used in stack context with non-temporary
// input parameters (and not modified in the meantime).
//
// YOU HAVE BEEN WARNED!

// This is modeled after RAII pattern, construction order does matter!

class namedmutex
{
public:
	class mx_name_space: public lockable,
			public std::map<std::string, std::pair<unsigned, lockable>>
	{
	};
	namedmutex(mx_name_space& lockSpace, const std::string &keyname) :
			m_space(lockSpace), m_keyname(keyname)
	{
		lockguard g(lockSpace);
		auto res = lockSpace.find(keyname);
		if (res != lockSpace.end())
		{
			m_myMutex = &res->second.second;
			res->second.first++;
			return;
		}
		auto x = lockSpace.insert(
				std::make_pair(keyname, std::make_pair((unsigned) 1, lockable())));
		m_myMutex = &x.first->second.second;
	}
	virtual ~namedmutex()
	{
		lockguard g(m_space);
		auto res = m_space.find(m_keyname);
		if (res == m_space.end()) // possible? needed?
			return;
		if (--res->second.first)
			return;
		m_space.erase(m_keyname);
	}
	operator lockable&()
	{
		return *m_myMutex;
	}


	private:
	lockable *m_myMutex;
	mx_name_space& m_space;
	const std::string& m_keyname;
};

extern namedmutex::mx_name_space g_noTruncateLocks;

#endif /* NAMEDMUTEX_H_ */
