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
#include "evabase.h"
#include "dnsbase.h"
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
static const unsigned DNS_ERROR_KEEP_MAX_TIME = 15;
static const string dns_error_status_prefix("503 DNS error - ");

static string make_dns_key(const string & sHostname, const string &sPort)
{
	return sHostname + ":" + sPort;
}

// using non-ordered map because of iterator stability, needed for expiration queue;
// something like boost multiindex would be more appropriate if there was complicated
// ordering on expiration date but that's not the case; OTOH could also use a multi-key
// index instead of g_active_resolver_index but then again, when the resolution is finished,
// that key data becomes worthless.
map<string,CAddrInfoPtr> dns_cache;
deque<decltype(dns_cache)::iterator> dns_exp_q;

// descriptor of a running DNS lookup, passed around with libevent callbacks
struct tDnsResContext
{
	string sHost, sPort;
	list<tDnsBase::tDnsResultReporter> cbs;
};
unordered_map<string,tDnsResContext*> g_active_resolver_index;


/**
 * Trash old entries and keep purging until there is enough space for at least one new entry.
 */
void CAddrInfo::clean_dns_cache()
{
	if(cfg::dnscachetime <= 0)
		return;
	auto now=GetTime();
	ASSERT(dns_cache.size() == dns_exp_q.size());
	while(dns_cache.size() >= DNS_CACHE_MAX-1
			|| (!dns_exp_q.empty() && dns_exp_q.front()->second->m_expTime >= now))
	{
		dns_cache.erase(dns_exp_q.front());
		dns_exp_q.pop_front();
	}
}

void CAddrInfo::cb_dns(int rc, struct evutil_addrinfo *results, void *arg)
{
	// take ownership
	unique_ptr<tDnsResContext> args((tDnsResContext*)arg);
	arg = nullptr;
	try
	{
		g_active_resolver_index.erase(make_dns_key(args->sHost, args->sPort));
		auto ret = std::shared_ptr<CAddrInfo>(new CAddrInfo);
		tDtorEx invoke_cbs([&args, &ret]() { for(auto& it: args->cbs) it(ret);});

		switch (rc)
		{
		case 0:
			break;
		case EAI_AGAIN:
		case EAI_MEMORY:
		case EAI_SYSTEM:
			ret->m_expTime = 0; // expire this ASAP and retry
			ret->m_sError = "504 Temporary DNS resolution error";
			return;
		default:
			ret->m_expTime = GetTime() + std::min(cfg::dnscachetime, (int) DNS_ERROR_KEEP_MAX_TIME);
			ret->m_sError = dns_error_status_prefix + evutil_gai_strerror(rc); //   fmt_error(rc); //"If this refers to a configured cache repository, please check the corresponding configuration file");
			return;
		}
		ret->m_rawInfo = results;
#ifdef DEBUG
		for (auto p = ret->m_rawInfo; p; p = p->ai_next)
			std::cerr << formatIpPort(p) << std::endl;
#endif
		// find any suitable-looking entry and keep a pointer to it faster lookup
		for (auto pCur = ret->m_rawInfo; pCur && !ret->m_tcpAddrInfo; pCur = pCur->ai_next)
		{
			if (pCur->ai_socktype == SOCK_STREAM && pCur->ai_protocol == IPPROTO_TCP)
				ret->m_tcpAddrInfo = pCur;
		}
		if (ret->m_tcpAddrInfo)
			ret->m_expTime = GetTime() + cfg::dnscachetime;
		else
		{
			// nothing found? Report a common error then.
			ret->m_expTime = GetTime() + std::min(cfg::dnscachetime, (int) DNS_ERROR_KEEP_MAX_TIME);
			ret->m_sError = dns_error_status_prefix + evutil_gai_strerror(EAI_SYSTEM);
		}
		if (cfg::dnscachetime > 0) // keep a copy for other users
		{
			clean_dns_cache();
			auto newIt = dns_cache.emplace(make_dns_key(args->sHost, args->sPort), ret);
			dns_exp_q.push_back(newIt.first);
		}
	}
	catch(...)
	{
		// nothing above should actually throw, but if it does, make sure to not keep wild pointers
		g_active_resolver_index.clear();
	}
}

CAddrInfo::~CAddrInfo()
{
	if (m_rawInfo) evutil_freeaddrinfo(m_rawInfo);
	m_tcpAddrInfo = m_rawInfo = nullptr;
}
}
