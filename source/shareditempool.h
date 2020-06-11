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
#include <queue>
#include <utility>

#include "atimer.h"
#include "sut.h"

namespace acng
{

/**
 * helper structures which stores some assets in a dictionary, where:
 * a) elements are reference-counted
 * b) elements are stored as map sub-elements, no extra indirection to allocated additional elements
 * c) refcounting happens through guardian objects - whenever the last reference keeper is gone, the entry is removed from the map
 *
 * This is very simple and mostly usable for non-complex items.
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

#if 0
// XXX: that was a lousy attempt, and it misses the actual requirements
// keep that bit-orable with true/false
enum class EInvalPolicy
{
	/**
	 * No extra timer creation. timerCreator parameter can be invalid.
	 */
	NO_TIMER = 2,

			/**
			 * Set the timer on the first use, forcibly remove from lookup dic after that timeout
			 */
	ON_FIRST_USE = 4,
	/**
	 * Start timer after the last user is gone but keep the element alive until timeout.
	 * If a new user appears who takes reference of the object, the cleanup will be postponed again.
	 */
	ON_LAST_USE = 8
} m_policy;
template<typename Tkey, typename Tvalue, EInvalPolicy policy>
class TExpiringItemPool : public tLintRefcounted
{
	SUTPRIVATE:
	struct TElement
	{
		unsigned use_count;
		time_t expire_at;
		Tvalue value;
	};
	std::map<Tkey, TElement> index;
public:
	atimerfac m_timerFactory;
	std::time_t m_timeoutSeconds;
	ATimerPtr m_timer;
	TExpiringItemPool(atimerfac timerCreator = atimerfac(), std::time_t expire_timeout = 0)
	: m_timerFactory(timerCreator), m_timeoutSeconds(expire_timeout)
	{
	}
	class TReference
	{
		lint_ptr<TExpiringItemPool> pool_ref;
		decltype(TExpiringItemPool::index)::iterator item_ref;
public:
		TReference() =default;
		TReference(decltype(item_ref) i,decltype(pool_ref) r):
			pool_ref(r),
			item_ref(i)
		{
			item_ref->second.use_count++;
		}

		// reference to value, for reading or modifying
		Tvalue& value() noexcept
		{
			return item_ref->second.value;
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
			item_ref.use_count++;
		}
		void reset()
		{
			if(!pool_ref)
				return;
			item_ref->second.use_count--;
			if(item_ref->second.use_count == 0)
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
	SUTPRIVATE:
	std::queue<TReference> m_expireQueue;
public:
	/**
	 * Disable all timeout functionality and drop all sentinels, triggering the cleanup jobs implicitly.
	 */
	void dispose()
	{
		m_policy = EInvalPolicy::NO_TIMER;
		m_timer.reset();
		decltype(m_expireQueue)().swap(m_expireQueue); // clean() replacement
	}
SUTPRIVATE:
	virtual std::time_t purge()
	{
		auto now = GetTime();
		while (!m_expireQueue.empty())
		{
			auto xtime = m_expireQueue.front().item_ref->second.expire_at;
#error beide bedingungen scheisse beim recycle, dann kann vorne $irgendwas stehen
			if(0 == xtime)
				continue;
			if (xtime > now)
				return xtime - now;
			m_expireQueue.pop();
		}
		return 0;
	}
	virtual TReference&& armTimer(TReference&& entry)
	{
		if (!m_timer)
		{
			m_timer = m_timerFactory([x = acng::lint_ptr(this)]()
			{ return x->purge();});
		}
		entry.item_ref->second.expire_at = GetTime() + m_timeoutSeconds;
		if(m_expireQueue.empty()) // first one? if not then the timer is already pending
			m_timer->rearm(m_timeoutSeconds);
		m_expireQueue.emplace(entry);
		return move(entry);
	}
public:
	TReference Get(const Tkey& key, bool* reportNew)
	{
		auto it = index.find(key);
		auto was_there = it != index.end();
		if(reportNew)
			*reportNew = !was_there;
		if(was_there)
		{
			// invalidate the possible active timer task
			if(policy == EInvalPolicy::ON_FIRST_USE)
			{
				it->second.expire_at = 0;
			}

			return TReference(it, lint_ptr<TSharedItemPool>(this));
		}
		// ok, is new, need to start the expiration timer?
		it = index.emplace(std::make_pair(key, {0, 0, Tvalue()})).first;
		if(policy == EInvalPolicy::ON_FIRST_USE)
		{
			return armTimer(TReference(it, lint_ptr<TSharedItemPool>(this)));
		}
		return TReference(it, lint_ptr<TSharedItemPool>(this));
	}
	inline TReference Get(const Tkey& key) { return Get(key, nullptr); }
	inline TReference Get(const Tkey& key, bool& was_new) { return Get(key, &was_new); }
};



}
#endif

#endif /* SOURCE_SHAREDITEMPOOL_H_ */
