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

class namedmutex
{
public:
   struct refentry
   {
      unsigned int nRefCount;
      lockable mx;
      /*!
       *
       * Locked for mmap means special behavior: the thread can be killed, and
       * the implementation should care about how to continue from this state.
       * */
      pthread_t threadref;
      bool lockedForMmap;
   };
	class mx_name_space: public lockable,
			public std::map<std::string, refentry>
	{
	};
  inline void init(const std::string &keyname)
  {
#error erneueter init sollte den counter wieder freigaben... ctor code nach dinit verlagern?
		lockguard g(m_space);
		auto res = m_space.find(keyname);
		if (res != m_space.end())
		{
			pEntry = &res->second;
			pEntry->nRefCount++;
			return;
		}
		auto x = m_space.insert(
				std::make_pair(keyname, refentry({1, lockable(), pthread_self(), false})));
		pEntry = &x.first->second;
  }
	namedmutex(mx_name_space& lockSpace) :
  m_space(lockSpace)
	{
	}
	namedmutex(mx_name_space& lockSpace, const std::string &keyname) :
  m_space(lockSpace)
	{
  init(keyname);
	}
	virtual ~namedmutex()
	{
    if(!pEntry) 
    return;

		lockguard g(m_space);
		auto res = m_space.find(m_keyname);
		if (res == m_space.end()) // possible? needed?
			return;
		if (--res->second.nRefCount)
			return;
		m_space.erase(m_keyname);
	}

  void lock(bool bForMmap) { 
     pEntry->mx.lock();
     pEntry->threadref=pthread_self();
     pEntry->lockedForMmap=bForMmap;
  }
  void unlock() {
     if(!pEntry)
        return;
     pEntry->lockedForMmap=false;
     pEntry->mx.unlock(); 
  }
  struct guard {
  namedmutex &m_mx;
   guard(namedmutex &mx, bool bForMmap) : m_mx(mx) { 
      mx.lock(bForMmap);
   }
   ~guard() { m_mx.unlock(); }
  };


	private:
	refentry *pEntry = nullptr;
	mx_name_space& m_space;
	std::string m_keyname;
};

extern namedmutex::mx_name_space g_noTruncateLocks;

#endif /* NAMEDMUTEX_H_ */
