
#include "meta.h"
#include "caddrinfo.h"
#include "sockio.h"
#include "acfg.h"
#include "debug.h"
#include "lockable.h"
#include "cleaner.h"

#include <deque>
#include <memory>

using namespace std;

namespace acng
{
static const unsigned DNS_CACHE_MAX=255;

static string make_dns_key(const string & sHostname, const string &sPort)
{
	return sHostname + "/" + sPort;
}

base_with_condition dnsCacheCv;

map<string,CAddrInfoPtr> dnsCache;
deque<decltype(dnsCache)::iterator> dnsAddSeq;

bool CAddrInfo::ResolveTcpTarget(const string & sHostname, const string &sPort,
		string & sErrorBuf,
		const evutil_addrinfo* pHints,
		bool & bTransientError)
{
	LOGSTART2("CAddrInfo::Resolve", "Resolving " << sHostname);

	sErrorBuf.clear();
	auto filter_specific = (cfg::conprotos[0] != PF_UNSPEC && cfg::conprotos[1] == PF_UNSPEC);
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
		// XXX: maybe also evaluate errno in case of EAI_SYSTEM
		sErrorBuf = "503 DNS error for " + sHostname + ":" + sPort + " : " + evutil_gai_strerror(rc);
		if(sfx)	sErrorBuf += string("(") + sfx + ")";
		LOG(sfx);
		return false;
	};

	Reset();

	int r = evutil_getaddrinfo(sHostname.empty() ? nullptr : sHostname.c_str(),
			sPort.empty() ? nullptr : sPort.c_str(),
			pHints ? pHints : &default_connect_hints,
			&m_rawInfo);
	switch(r)
	{
	case 0: break;
	case EAI_AGAIN:
	case EAI_MEMORY:
	case EAI_SYSTEM:
		bTransientError = true;
		__just_fall_through;
	default:
		return ret_error("If this refers to a configured cache repository, please check the corresponding configuration file", r);
	}
#ifdef DEBUG
	for(auto p=m_rawInfo; p; p=p->ai_next)
		std::cerr << formatIpPort(p) << std::endl;
#endif
	// find any suitable-looking entry and keep a pointer to it faster lookup
	for (auto pCur=m_rawInfo; pCur; pCur = pCur->ai_next)
	{
		if (pCur->ai_socktype != SOCK_STREAM || pCur->ai_protocol != IPPROTO_TCP)
			continue;
		m_tcpAddrInfo = pCur;
		return true;
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

CAddrInfoPtr CAddrInfo::CachedResolve(const string & sHostname, const string &sPort, string &sErrorMsgBuf)
{
	bool dummy_run = sHostname.empty() && sPort.empty();
	bool bTransientError = false;
	auto resolve_now = [&]()
			{
		auto ret = make_shared<CAddrInfo>();
		if(! ret->ResolveTcpTarget(sHostname, sPort, sErrorMsgBuf, nullptr, bTransientError))
			ret.reset();
		return ret;
	};
	if (!cfg::dnscachetime && !dummy_run)
		return resolve_now();

	auto dnsKey = make_dns_key(sHostname, sPort);
	auto now(GetTime());

	lockuniq lg(dnsCacheCv);

	// clean all expired entries
	unsigned n(0);
	while(!dnsAddSeq.empty() && dnsAddSeq.front()->second && dnsAddSeq.front()->second->m_expTime <= now)
	{
		// just to be safe in case anyone waits for it
		if(!dnsAddSeq.front()->second->m_sError)
			dnsAddSeq.front()->second->m_sError.reset(new string("504 DNS Cache Timeout"));
		dnsCache.erase(dnsAddSeq.front());
		dnsAddSeq.pop_front();
		++n;
	}
	if(n) dnsCacheCv.notifyAll();

	if (dummy_run)
		return CAddrInfoPtr();

	if (dnsCache.size() >= DNS_CACHE_MAX)
	{
		// something is fishy, too long exp. time? Just pass through then
		lg.unLock();
		return resolve_now();
	}

	auto insres = dnsCache.emplace(dnsKey, make_shared<CAddrInfo>());
	auto p = insres.first->second;
	auto is_ours(insres.second);
	if (is_ours)
	{
		dnsAddSeq.push_back(insres.first);
		p->m_expTime = now + cfg::dnscachetime;
	}
	else // reuse the results from another thread
	{
		while(true)
		{
			if(p->m_sError)
			{
				sErrorMsgBuf = * p->m_sError;
				return CAddrInfoPtr();
			}
			if(p->m_expTime <= MAX_VAL(time_t))
				return p;
			dnsCacheCv.wait(lg);
		}
	}
	lg.unLock();
	auto resret = p->ResolveTcpTarget(sHostname, sPort, sErrorMsgBuf, nullptr, bTransientError);
	lg.reLock();
	if(resret)
	{
		p->m_expTime = now + cfg::dnscachetime;
		dnsCacheCv.notifyAll();
		return p;
	}
	// or handle errors, keep them for observers
	p->m_sError.reset(new string(sErrorMsgBuf));
	// for permanent failures, also keep them in cache for a while
	if(!bTransientError)
		p->m_expTime = now + cfg::dnscachetime;
	dnsCacheCv.notifyAll();
	return p;
}

#if 0
void DropDnsCache()
{
#warning check all usage
	lockguard g(dnsCacheCv);
	dnsCleanupQ.clear();
	dnsCache.clear();
}
#endif

}
