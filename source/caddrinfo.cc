
#include "caddrinfo.h"
#include "acfg.h"
#include "debug.h"
#include "cleaner.h"

#include <deque>
#include <memory>
#include <thread>
using namespace std;

#define DNS_MAX_PAR 8

namespace acng
{
static const unsigned DNS_CACHE_MAX=255;

static string make_dns_key(const string & sHostname, const string &sPort)
{
	return sHostname + "/" + sPort;
}

map<string,CAddrInfoPtr> dnsCache;
base_with_mutex dnsCacheMx;
deque<decltype(dnsCache)::iterator> dnsAddSeq;

void CAddrInfo::ResolveTcpTarget(const string & sHostname, const string &sPort,
		const evutil_addrinfo* pHints,
		bool & bTransientError)
{
	LOGSTARTFUNCx(sHostname);
	auto filter_specific = (cfg::conprotos[0] != PF_UNSPEC && cfg::conprotos[1] == PF_UNSPEC);
	dbgline;
	evutil_addrinfo default_connect_hints =
	{
		// we provide plain port numbers, no resolution needed
		// also return only probably working addresses
		AI_NUMERICSERV | AI_ADDRCONFIG,
		filter_specific ? cfg::conprotos[0] : PF_UNSPEC,
		SOCK_STREAM, IPPROTO_TCP,
		0, nullptr, nullptr, nullptr
	};

	auto ret_error = [&](const char* sfx, int rc)
			{
		dbgline;
		// XXX: maybe also evaluate errno in case of EAI_SYSTEM
		m_sError = "503 DNS error for " + sHostname + ":" + sPort + " : " + evutil_gai_strerror(rc);
		if(sfx)
			m_sError += string("(") + sfx + ")";
		LOG(sfx);
		m_tcpAddrInfo = nullptr;
		if(!m_rawInfo) return;
		evutil_freeaddrinfo(m_rawInfo);
		m_rawInfo = nullptr;
	};

	Reset();

	static atomic_int dns_overload_limiter(0);
	while(dns_overload_limiter > DNS_MAX_PAR)
		this_thread::sleep_for(1s);
	dns_overload_limiter++;
	int r = evutil_getaddrinfo(sHostname.empty() ? nullptr : sHostname.c_str(),
			sPort.empty() ? nullptr : sPort.c_str(),
			pHints ? pHints : &default_connect_hints,
			&m_rawInfo);
	dns_overload_limiter--;

	switch(r)
	{
	case 0: break;
	case EAI_AGAIN:
	case EAI_MEMORY:
	case EAI_SYSTEM:
		dbgline;
		bTransientError = true;
		__just_fall_through;
	default:
		dbgline;
		return ret_error("If this refers to a configured cache repository, please check the corresponding configuration file", r);
	}
#ifdef DEBUG
	for(auto p=m_rawInfo; p; p=p->ai_next)
		std::cerr << formatIpPort(p) << std::endl;
#endif
	// find any suitable-looking entry and keep a pointer to it faster lookup
	for (auto pCur=m_rawInfo; pCur; pCur = pCur->ai_next)
	{
		dbgline;
		if (pCur->ai_socktype != SOCK_STREAM || pCur->ai_protocol != IPPROTO_TCP)
			continue;
		m_tcpAddrInfo = pCur;
		dbgline;
		return;
	}

	return ret_error("no suitable target service", EAI_SYSTEM);
}

void CAddrInfo::Reset()
{
	if (m_rawInfo)
		evutil_freeaddrinfo(m_rawInfo);
	m_tcpAddrInfo = m_rawInfo = nullptr;
}

CAddrInfo::~CAddrInfo()
{
	Reset();
}

CAddrInfoPtr CAddrInfo::CachedResolve(const string & sHostname, const string &sPort)
{
	LOGSTARTFUNCxs(sHostname, sPort);
	CAddrInfoPtr ret;

	bool bTransientError = false;

	if(!cfg::dnscachetime)
	{
		ret = make_shared<CAddrInfo>();
		ret->ResolveTcpTarget(sHostname, sPort, nullptr, bTransientError);
		return ret;
	}
	auto dnsKey = make_dns_key(sHostname, sPort);
	auto now(GetTime());

	{
		lockguard lg(dnsCacheMx);
		auto& xref = dnsCache[dnsKey];
		if(xref)
			ret = xref;
		else
			ret = xref =  make_shared<CAddrInfo>();
	}

	bool bKickFromTheMap = false;
	time_t expireWhen = END_OF_TIME;
	{
		lockguard g(*ret);
		// ok, either we did resolve it or someone else?
		if (ret->HasResult())
			return ret;

		// ok, our thread is responsible
		ret->ResolveTcpTarget(sHostname, sPort, nullptr, bTransientError);

		bKickFromTheMap = ret->HasError() && !bTransientError; // otherwise: cache an error hint for permanent errors

		if(!bKickFromTheMap)
			expireWhen = ret->m_expTime = now + cfg::dnscachetime;
	}

	if(bKickFromTheMap)
	{
		lockguard lg(dnsCacheMx);
		dnsCache.erase(dnsKey);
	}
	else
		cleaner::GetInstance().ScheduleFor(expireWhen, cleaner::eType::DNS_CACHE);

	return ret;
}

time_t expireDnsCache()
{
	LOGSTARTFUNCs;
	lockguard lg(dnsCacheMx);
	// keep all which expire after now, plus a few second of extra cleanup time (kill sooner) to catch all made by a request burst at once
	auto dropBefore = GetTime() + 5;
	auto ret = END_OF_TIME;
	for(auto it = dnsCache.begin(); it!= dnsCache.end();)
	{
		if(!it->second || it->second->GetExpirationTime() <= dropBefore)
			it = dnsCache.erase(it);
		else
		{
			ret = std::min(ret, it->second->GetExpirationTime());
			++it;
		}
	}
	LOGRET(ret);
}

}
