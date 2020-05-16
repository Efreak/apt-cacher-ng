#include "meta.h"
#include "dnsbase.h"

#include <future>

using namespace std;
namespace acng
{

// this shall remain global and forever, for last-resort notifications
SHARED_PTR<CAddrInfo> fail_hint=std::make_shared<CAddrInfo>("503 Fatal system error within apt-cacher-ng processing");

SHARED_PTR<CAddrInfo> tDnsBase::Resolve(cmstring & sHostname, cmstring &sPort)
{
	promise<shared_ptr<CAddrInfo> > reppro;
	Resolve(sHostname, sPort, [&reppro](shared_ptr<CAddrInfo> result) { reppro.set_value(result); });
	auto res(reppro.get_future().get());
	return res ? res : fail_hint;
}

void tDnsBase::Resolve(cmstring & sHostname, cmstring &sPort, tDnsResultReporter rep) noexcept
{

#warning catch exceptions and handle via rep
	auto temp_ctx = new tDnsResContext {
		sHostname,
		sPort,
		list<CAddrInfo::tDnsResultReporter> {move(rep)}
	};

	auto cb_invoke_dns_res = [temp_ctx](bool canceled)
	{
		auto args = unique_ptr<tDnsResContext>(temp_ctx); //temporarily owned here
		if(!args || args->cbs.empty() || !(args->cbs.front())) return; // heh?
		LOGSTART2s("cb_invoke_dns_res", temp_ctx->sHost);

		if(canceled || evabase::in_shutdown)
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
		auto resIt = g_active_resolver_index.find(key);
		// join the waiting crowd, move all callbacks to there...
		if(resIt != g_active_resolver_index.end())
		{
			resIt->second->cbs.splice(resIt->second->cbs.end(), args->cbs);
			return;
		}
		// ok, invoke a completely new DNS lookup operation
		g_active_resolver_index[key] = args.get();
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
	};

	evabase::Post(move(cb_invoke_dns_res));
}
}
