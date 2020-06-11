#include "meta.h"

#include <deque>
#include <memory>
#include <list>
#include <unordered_map>
#include <future>
#include <queue>

#include "caddrinfo.h"
#include "sockio.h"
#include "acfg.h"
#include "debug.h"
#include "lockable.h"
#include "acngbase.h"
#include <event2/dns.h>
using namespace std;

/**
 * The CAddrInfo has multiple jobs:
 * - cache the resolution results for a specific time span
 * - in case of parallel ongoing resolution requests, coordinate the wishes so that the result is reused (and all callers are notified)
 */

namespace acng
{
static const unsigned DNS_CACHE_MAX = 255;
//static const unsigned DNS_ERROR_KEEP_MAX_TIME = 15;
static const string dns_error_status_prefix("503 DNS error - ");

class tDnsBaseImpl;
class CAddrInfoImpl;
typedef acng::lint_ptr<CAddrInfoImpl> CAddrInfoImplPtr;

// descriptor of a running DNS lookup, passed around with libevent callbacks
struct tActiveResolutionContext : tLintRefcounted
{
	// yes, it builds a reference cycle which is broken by the C callback eventually
	lint_ptr<CAddrInfoImpl> what;
	std::list<tDnsResultReporter> requester_actions;
	tActiveResolutionContext(lint_ptr<CAddrInfoImpl> _what, tDnsResultReporter first_rep)
	: what(_what), requester_actions({move(first_rep)})
	{
	}
};

using tRawAddrinfoPtr = resource_owner<evutil_addrinfo*, evutil_freeaddrinfo, nullptr>;

static std::string make_dns_key(const std::string & sHostname, const std::string &sPort)
{
	return sHostname + ":" + sPort;
}

class ACNG_API CAddrInfoImpl : public CAddrInfo
{
	friend class tDnsBaseImpl;

	// valid as long as the resolution is ongoing
	lint_ptr<tActiveResolutionContext> current_resolver_activity;

	// resolution results (or error hint for caching)
	std::string m_sError;

	// raw returned data from getaddrinfo
	tRawAddrinfoPtr m_rawInfo;
	// shortcut for iterators, first in the list with TCP target
	evutil_addrinfo *m_tcpAddrInfo = nullptr;

	void setError(int eaiError)
	{
		if(!eaiError)
			return;
		m_sError = dns_error_status_prefix + evutil_gai_strerror(eaiError);
	}

	void setResult(tRawAddrinfoPtr&& results)
	{
		try
		{
			m_rawInfo.reset(move(results));
#ifdef DEBUG
			for (auto p = *m_rawInfo; p; p = p->ai_next)
				std::cerr << formatIpPort(p) << std::endl;
#endif
			// find any suitable-looking entry and keep a pointer to it faster lookup
			for (auto pCur = *m_rawInfo; pCur && !m_tcpAddrInfo; pCur = pCur->ai_next)
			{
				if (pCur->ai_socktype == SOCK_STREAM && pCur->ai_protocol == IPPROTO_TCP)
					m_tcpAddrInfo = pCur;
			}
		} catch (const exception &ex)
		{
			m_sError = dns_error_status_prefix + ex.what();
		}
	}
public:

	const evutil_addrinfo* getTcpAddrInfo() const
	{
		return m_tcpAddrInfo;
	}
	const std::string& getError() const
	{
		return m_sError;
	}

	// C-style callback for libevent
	static void cb_dns(int result, struct evutil_addrinfo *results, void *arg);

	explicit CAddrInfoImpl(const char *szErrorMessage) :
			m_sError(szErrorMessage ? szErrorMessage : "")
	{
	}

	CAddrInfoImpl()
	{
	}

	explicit CAddrInfoImpl(int eaiError)
	{
		setError(eaiError);
	}
};

// this shall remain global and forever, for last-resort notifications
CAddrInfoPtr RESPONSE_FAIL = CAddrInfoPtr(new CAddrInfoImpl("503 Fatal system error within apt-cacher-ng processing"));
CAddrInfoPtr RESPONSE_CANCELED = CAddrInfoPtr(new CAddrInfoImpl(EAI_CANCELED));

class ACNG_API tDnsBaseImpl: public tDnsBase, public std::enable_shared_from_this<tDnsBaseImpl>
{
public:
	evdns_base *m_evdnsbase;
	tSysRes &m_src;

	explicit tDnsBaseImpl(tSysRes &src) : tDnsBase(), m_src(src)
	{
		m_evdnsbase = evdns_base_new(src.GetEventBase(), EVDNS_BASE_INITIALIZE_NAMESERVERS);
	}
	~tDnsBaseImpl()
	{
		evdns_base_free(m_evdnsbase, true);
	}

	static std::shared_ptr<tDnsBase> Create(event_base *evbase);

	// async. DNS resolution on IO thread. Reports result through the reporter.
	void Resolve(cmstring &sHostname, cmstring &sPort, tDnsResultReporter) noexcept;
	// like above but blocking
	CAddrInfoPtr Resolve(cmstring &sHostname, cmstring &sPort) override
	{
		promise<CAddrInfoPtr> reppro;
		Resolve(sHostname, sPort, [&reppro](CAddrInfoPtr result) { reppro.set_value(result);} ); // @suppress("Invalid arguments")
		auto res(reppro.get_future().get());
		return res ? res : RESPONSE_FAIL;
	}

	// using non-ordered map because of iterator stability, needed for expiration queue;
	// something like boost multiindex would be more appropriate if there was complicated
	// ordering on expiration date but that's not the case; OTOH could also use a multi-key
	// index instead of g_active_resolver_index but then again, when the resolution is finished,
	// that key data becomes worthless.
	map<string, CAddrInfoImplPtr> dns_cache;
	queue<pair<time_t, decltype(dns_cache)::iterator>> dns_exp_q;

	// add or fetch an existing entry. If added as fresh, arm the timer for expiration
	CAddrInfoImplPtr GetOrAdd2Cache(cmstring& key)
	{
		if(cfg::dnscachetime < 0)
			return make_lptr<CAddrInfoImpl>();

		auto res = dns_cache.emplace(key, CAddrInfoImplPtr());
		if(res.second)
			res.first->second.reset(new CAddrInfoImpl);
		return res.first->second;
#warning Implement cleanup with timer
	}

};

void ACNG_API AddDefaultDnsBase(tSysRes &src)
{
	src.dnsbase.reset(new tDnsBaseImpl(src));
}

void tDnsBaseImpl::Resolve(cmstring &sHostname, cmstring &sPort, tDnsResultReporter rep) noexcept
{
	m_src.fore->PostOrRun([sHostname, sPort, rep = move(rep), me = shared_from_this()] // @suppress("Invalid arguments")
						   (bool canceled)
						   {

				if(canceled) return rep(RESPONSE_CANCELED);

				try
				{
					auto key=make_dns_key(sHostname, sPort);
					// try using old cached entry, or get a new one
					auto pImpl = me->GetOrAdd2Cache(key);
					if(pImpl->current_resolver_activity)
					{
						// join the waiting crowd, move all callbacks to there...
						pImpl->current_resolver_activity->requester_actions.emplace_back(move(rep));
						return;
					}
					pImpl->current_resolver_activity.reset(new tActiveResolutionContext(pImpl, move(rep)));
					static bool filter_specific = (cfg::conprotos[0] != PF_UNSPEC && cfg::conprotos[1] == PF_UNSPEC);
					static const evutil_addrinfo default_connect_hints =
					{
						// we provide plain port numbers, no resolution needed
						// also return only probably working addresses
						AI_NUMERICSERV | AI_ADDRCONFIG,
						filter_specific ? cfg::conprotos[0] : PF_UNSPEC,
						SOCK_STREAM, IPPROTO_TCP,
						0, nullptr, nullptr, nullptr
					};
					pImpl->current_resolver_activity->__inc_ref(); // to be reverted in the callback
					evdns_getaddrinfo(me->m_evdnsbase, sHostname.empty() ? nullptr : sHostname.c_str(),
							sPort.empty() ? nullptr : sPort.c_str(),
							&default_connect_hints, CAddrInfoImpl::cb_dns, pImpl->current_resolver_activity.get());
				}
				catch(...)
				{
					rep(RESPONSE_FAIL);
				}
			});
}


void CAddrInfoImpl::cb_dns(int rc, struct evutil_addrinfo *resultsRaw, void *argRaw)
{
	// take ownership of result in any case, to clear or move it around
	tRawAddrinfoPtr results(resultsRaw);

	if (!argRaw)
		return; // not good but not fixable :-(

	// revert the reference offset, take over and remove the resolution context reference
	auto ctx = lint_ptr<tActiveResolutionContext>((tActiveResolutionContext*)argRaw, false);
	ctx->what->current_resolver_activity.reset();

	auto report = [&](const CAddrInfoPtr& ret)
			{
		for(auto& it: ctx->requester_actions)
		try {
			it(ret);
		} catch (...) {
		}
	};

	// now dispatch actual response
	try
	{
		if (rc)
			ctx->what->setError(rc);
		else if (resultsRaw)
			ctx->what->setResult(move(results));
		auto wtf = static_lptr_cast<CAddrInfo>(ctx->what);
		report(wtf);
		return;
	}
	catch (...)
	{
		report(RESPONSE_FAIL);
		return;
	}

	// failed to provide information nor error? wtf?
	report(RESPONSE_FAIL);
}
}
