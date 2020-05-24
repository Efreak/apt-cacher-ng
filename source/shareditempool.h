/*
 * shareditempool.h
 *
 *  Created on: 23.05.2020
 *      Author: Eduard Bloch
 */

#ifndef SOURCE_SHAREDITEMPOOL_H_
#define SOURCE_SHAREDITEMPOOL_H_

#include "acsmartptr.h"
#include <map>
#include <utility>

#include "sut.h"

namespace acng
{

/**
 * helper structures which stores some assets in a dictionary, where:
 * a) elements are reference-counted
 * b) elements are stored as map sub-elements, no extra indirection to allocated additional elements
 * c) refcounting happens through guardian objects - whenever the last reference keeper is gone, the entry is removed from the map
 */
template<typename Tkey, typename Tvalue>
class TSharedItemPool : public tLintRefcounted
{
	SUTPRIVATE:

	std::map<Tkey, std::pair<unsigned, Tvalue>> index;
public:
	class TReference
	{
		lint_ptr<TSharedItemPool> pool_ref;
		decltype(TSharedItemPool::index)::iterator item_ref;
public:
		TReference() =default;

		TReference(decltype(item_ref) i,decltype(pool_ref) r):
			pool_ref(r),
			item_ref(i)
{
			item_ref->second.first++;
		}

		// reference to value, for reading or modifying
		Tvalue& value() noexcept
		{
			return item_ref->second.second;
		}

		TReference operator=(const TReference& src) noexcept
		{
			if(pool_ref && pool_ref == src.pool_ref && item_ref == src.item_ref)
			{
				// this was set before to the same element, or is even the same *this?
				return *this;
			}
			reset();
			pool_ref = src.pool_ref;
			item_ref = src.item_ref;
			item_ref.first++;
		}
		void reset()
		{
			if(!pool_ref)
				return;
			item_ref->second.first--;
			if(item_ref->second.first == 0)
			{
				pool_ref->index.erase(item_ref);
			}
			pool_ref.reset();
		}
		~TReference()
		{
			reset();
		}
	};
	TReference Get(const Tkey& key, bool* reportNew)
	{
		auto it = index.find(key);
		if(it != index.end())
		{
			if(reportNew)
				*reportNew = false;
			return TReference(it, lint_ptr<TSharedItemPool>(this) );
		}
		if(reportNew)
			*reportNew = true;
		auto iterAndInsertflag = index.emplace(std::make_pair(key, std::make_pair(0, Tvalue())));
		return TReference(iterAndInsertflag.first, lint_ptr<TSharedItemPool>(this) );
	}
	inline TReference Get(const Tkey& key) { return Get(key, nullptr); }
	inline TReference Get(const Tkey& key, bool& was_new) { return Get(key, &was_new); }
};
}
#endif /* SOURCE_SHAREDITEMPOOL_H_ */
