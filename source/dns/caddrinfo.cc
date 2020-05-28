#include "meta.h"

#include <deque>
#include <memory>
#include <list>
#include <unordered_map>
#include <future>

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

// descriptor of a running DNS lookup, passed around with libevent callbacks
struct tActiveResolutionContext
{
	std::string sHostPortKey;
	std::weak_ptr<tDnsBaseImpl> pDnsBase;
};

using tRawAddrinfoPtr = resource_owner<evutil_addrinfo*, evutil_freeaddrinfo, nullptr>;

static std::string make_dns_key(const std::string & sHostname, const std::string &sPort)
{
	return sHostname + ":" + sPort;
}

class ACNG_API CAddrInfoImpl : public CAddrInfo
{
	// resolution results (or error hint for caching)
	std::string m_sError;
	//time_t m_expTime = MAX_VAL(time_t);

	// raw returned data from getaddrinfo
	tRawAddrinfoPtr m_rawInfo;
	// shortcut for iterators, first in the list with TCP target
	evutil_addrinfo *m_tcpAddrInfo = nullptr;

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
			m_sError(szErrorMessage)
	{
	}

	explicit CAddrInfoImpl(int eaiError)
	{
		if(!eaiError)
			return;
		m_sError = dns_error_status_prefix + evutil_gai_strerror(eaiError);
	}

	explicit CAddrInfoImpl(tRawAddrinfoPtr&& results)
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

};

// this shall remain global and forever, for last-resort notifications
std::shared_ptr<CAddrInfo> RESPONSE_FAIL = static_pointer_cast<CAddrInfo>(std::make_shared<CAddrInfoImpl>("503 Fatal system error within apt-cacher-ng processing"));
std::shared_ptr<CAddrInfo> RESPONSE_CANCELED = static_pointer_cast<CAddrInfo>(make_shared<CAddrInfoImpl>(EAI_CANCELED));


class ACNG_API tDnsBaseImpl: public tDnsBase, public std::enable_shared_from_this<tDnsBaseImpl>
{
public:
	evdns_base *m_evdnsbase;
	// this must be regular map because the elements must not be moved in memory!
	std::map<std::string, std::list<tDnsResultReporter>> active_resolvers;
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
	std::shared_ptr<CAddrInfo> Resolve(cmstring &sHostname, cmstring &sPort)
	{
		promise<shared_ptr<CAddrInfo> > reppro;
		Resolve(sHostname, sPort, [&reppro](CAddrInfoPtr result) { reppro.set_value(result);} ); // @suppress("Invalid arguments")
		auto res(reppro.get_future().get());
		return res ? res : RESPONSE_FAIL;
	}

	// using non-ordered map because of iterator stability, needed for expiration queue;
	// something like boost multiindex would be more appropriate if there was complicated
	// ordering on expiration date but that's not the case; OTOH could also use a multi-key
	// index instead of g_active_resolver_index but then again, when the resolution is finished,
	// that key data becomes worthless.
	map<string, CAddrInfoPtr> dns_cache;
	deque<decltype(dns_cache)::iterator> dns_exp_q;

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
					// try using old cached entry, if possible
					if(cfg::dnscachetime > 0)
					{
						static time_t cache_purge_time = 0;
						auto now = GetTime();
#warning FIXME. Purging by timer should be done with a timer, not here
						if(now > cache_purge_time || me->dns_cache.size() > DNS_CACHE_MAX)
						{
							me->dns_cache.clear();
							cache_purge_time = now + cfg::dnscachetime;
						}
						else
						{
							auto caIt = me->dns_cache.find(key);
							if(caIt != me->dns_cache.end()) return rep(caIt->second);
						}
					}
					auto resIt = me->active_resolvers.find(key);
					// join the waiting crowd, move all callbacks to there...
					if(resIt != me->active_resolvers.end())
					{
						resIt->second.emplace_back(move(rep));
						return;
					}
					auto callbackHint = new tActiveResolutionContext { key, me };
					me->active_resolvers[key].emplace_back(move(rep));
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
					evdns_getaddrinfo(me->m_evdnsbase, sHostname.empty() ? nullptr : sHostname.c_str(),
							sPort.empty() ? nullptr : sPort.c_str(),
							&default_connect_hints, CAddrInfoImpl::cb_dns, callbackHint);
				}
				catch(...)
				{
					rep(RESPONSE_FAIL);
				}
			});
}


void CAddrInfoImpl::cb_dns(int rc, struct evutil_addrinfo *resultsRaw, void *argRaw)
{
	// take ownership in any case, to clear or move it around
	tRawAddrinfoPtr results(resultsRaw);
	auto ret = RESPONSE_FAIL;

	if (!argRaw)
		return; // not good but not fixable :-(

	auto ctx = std::unique_ptr<tActiveResolutionContext>((tActiveResolutionContext*)argRaw);
	auto dnsbase = ctx->pDnsBase.lock();
	if(!dnsbase)
		return;
	auto iter = dnsbase->active_resolvers.find(ctx->sHostPortKey);
	if(iter == dnsbase->active_resolvers.end())
		return;
	auto cbacks = move(iter->second);
	dnsbase->active_resolvers.erase(iter);
	// report something this in any case
	tDtorEx invoke_cbs([&]() { for(auto& it: cbacks) it(ret);}); // @suppress("Invalid arguments")

	// now dispatch actual response
	try
	{
		if(rc)
			ret = make_shared<CAddrInfoImpl>(rc);
		else if(!resultsRaw) // failed to provide information nor error? wtf?
			ret = RESPONSE_FAIL;
		else
			ret = make_shared<CAddrInfoImpl>(move(results));
	} catch (...)
	{
	}
}
}
