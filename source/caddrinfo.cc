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

static string make_dns_key(const string & sHostname, const string &sPort)
{
	return sHostname + ":" + sPort;
}
void cb_invoke_dns_res(int result, short what, void *arg);

// using non-ordered map because of iterator stability, needed for expiration queue
map<string,CAddrInfoPtr> dns_cache;
deque<decltype(dns_cache)::iterator> dns_exp_q;

// this shall remain global and forever, for last-resort notifications
SHARED_PTR<CAddrInfo> fail_hint=make_shared<CAddrInfo>("503 Fatal system error within apt-cacher-ng processing");

// descriptor of a running DNS lookup
struct tDnsResContext
{
	string sHost, sPort;
	list<CAddrInfo::tDnsResultReporter> cbs;
};
unordered_map<string,tDnsResContext*> g_active_resolvers;

/**
 * Trash old entries and make space for at least one new entry.
 */
void CAddrInfo::clean_dns_cache()
{
	if(cfg::dnscachetime <=0) return;
	auto now=GetTime();
	while(!dns_cache.empty())
	{
		if(dns_exp_q.front()->second->m_expTime > now && dns_cache.size()<DNS_CACHE_MAX-1 )
			break;
		dns_cache.erase(dns_exp_q.front());
		dns_exp_q.pop_front();
	}
}

SHARED_PTR<CAddrInfo> CAddrInfo::Resolve(cmstring & sHostname, cmstring &sPort)
{
	promise<CAddrInfoPtr> reppro;
	auto reporter = [&reppro](CAddrInfoPtr result) { reppro.set_value(move(result)); };
	Resolve(sHostname, sPort, move(reporter));
	auto res(move(reppro.get_future().get()));
	return res ? res : fail_hint;
}
void CAddrInfo::Resolve(cmstring & sHostname, cmstring &sPort, tDnsResultReporter rep)
{
	static const struct timeval tImmediately { 0, 0};
	auto repList = list<tDnsResultReporter> {move(rep)};
	auto ctx = new tDnsResContext {sHostname, sPort, move(repList)};
#warning fail hitn bei post fialure
	event_base_once(evabase::base, -1, EV_READ, cb_invoke_dns_res, ctx, &tImmediately);
}
void cb_invoke_dns_res(int result, short what, void *arg)
{
	unique_ptr<tDnsResContext> args((tDnsResContext*)arg);
	if(!args || args->cbs.empty() || !(args->cbs.front())) return; // heh?

	if(evabase::in_shutdown)
	{
		auto err_hint = make_shared<CAddrInfo>(evutil_gai_strerror(EAI_SYSTEM));
		args->cbs.front()(err_hint);
		return;
	}

	auto key=make_dns_key(args->sHost, args->sPort);
	if(cfg::dnscachetime > 0)
	{
		auto caIt = dns_cache.find(key);
		if(caIt != dns_cache.end())
		{
			args->cbs.front()(caIt->second);
			return;
		}
	}
	auto resIt = g_active_resolvers.find(key);
	// join the waiting crowd, mmove all callbacks to there...
	if(resIt != g_active_resolvers.end())
	{
		resIt->second->cbs.splice(resIt->second->cbs.end(), args->cbs);
		return;
	}
	// ok, invoke a completely new DNS lookup operation
	g_active_resolvers[key] = args.get();
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
	auto pRaw = args.release(); // to be owned by the operation
	evdns_getaddrinfo(evabase::dnsbase, pRaw->sHost.empty() ? nullptr : pRaw->sHost.c_str(),
			pRaw->sPort.empty() ? nullptr : pRaw->sPort.c_str(),
			&default_connect_hints, CAddrInfo::cb_dns, pRaw);
}

void CAddrInfo::cb_dns(int rc, struct evutil_addrinfo *results, void *arg)
{
	// take ownership
	unique_ptr<tDnsResContext> args((tDnsResContext*)arg);
	g_active_resolvers.erase(make_dns_key(args->sHost, args->sPort));

	auto ret = std::shared_ptr<CAddrInfo>(new CAddrInfo);
	auto invoke_cbs = [&args, &ret]() { for(auto& it: args->cbs) {it(ret);}};
	auto fmt_error = [](int rc) { return string("503 DNS error - ") + evutil_gai_strerror(rc); };

	switch(rc)
	{
	case 0: break;
	case EAI_AGAIN:
	case EAI_MEMORY:
	case EAI_SYSTEM:
		ret->m_expTime = 0; // expire this ASAP and retry
		ret->m_sError = "504 Temporary DNS resolution error";
		return invoke_cbs();
	default:
		ret->m_expTime = GetTime() + std::min(cfg::dnscachetime, (int) DNS_ERROR_KEEP_MAX_TIME);
		ret->m_sError = fmt_error(rc); //"If this refers to a configured cache repository, please check the corresponding configuration file");
		return invoke_cbs();
	}
	ret->m_rawInfo = results;
#ifdef DEBUG
	for(auto p=ret->m_rawInfo; p; p=p->ai_next)
		std::cerr << formatIpPort(p) << std::endl;
#endif
	// find any suitable-looking entry and keep a pointer to it faster lookup
	for (auto pCur = ret->m_rawInfo; pCur && !ret->m_tcpAddrInfo; pCur = pCur->ai_next)
	{
		if (pCur->ai_socktype == SOCK_STREAM && pCur->ai_protocol == IPPROTO_TCP)
			ret->m_tcpAddrInfo = pCur;
	}
	if(ret->m_tcpAddrInfo)
		ret->m_expTime = GetTime() + cfg::dnscachetime;
	else
	{
		// nothing found? Report a common error then.
		ret->m_expTime = GetTime() + std::min(cfg::dnscachetime, (int) DNS_ERROR_KEEP_MAX_TIME);
		ret->m_sError = fmt_error(EAI_SYSTEM); //"If this refers to a configured cache repository, please check the corresponding configuration file");
	}
	return invoke_cbs();
	if(cfg::dnscachetime > 0) // keep a copy for other users
	{
		clean_dns_cache();
		auto newIt = dns_cache.emplace(make_dns_key(args->sHost, args->sPort), ret);
		dns_exp_q.push_back(newIt.first);
	}
}

CAddrInfo::~CAddrInfo()
{
	if (m_rawInfo) evutil_freeaddrinfo(m_rawInfo);
	m_tcpAddrInfo = m_rawInfo = nullptr;
}
}
