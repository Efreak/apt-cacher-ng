
#include "meta.h"
#include "caddrinfo.h"
#include "sockio.h"
#include "acfg.h"
#include "debug.h"
#include "lockable.h"
#include "cleaner.h"

#include <list>


using namespace std;

namespace acng
{
static const unsigned DNS_CACHE_MAX=255;

static string make_dns_key(const string & sHostname, const string &sPort)
{
	auto len(sHostname);
	return sHostname + sPort + string((const char*)&sHostname, sizeof(sHostname));
}

base_with_condition dnsCacheCv;
map<string,CAddrInfoPtr> dnsCache;
list<decltype(dnsCache)::iterator> dnsCleanupQ;

bool CAddrInfo::ResolveTcpTarget(const string & sHostname, const string &sPort,
		string & sErrorBuf,
		const evutil_addrinfo* pHints)
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

	auto ret_error = [&](const char* sfx, int rc){
		sErrorBuf = "503 DNS error for " + sHostname + ":" + sPort + " : " + evutil_gai_strerror(rc);
		if(sfx)	sErrorBuf += string("(") + sfx + ")";
		m_psErrorMessage = new string(sErrorBuf);
		m_nResTime = RES_ERROR;
		LOG(sfx);
		return false;
	};

	Reset();

	int r = evutil_getaddrinfo(sHostname.empty() ? nullptr : sHostname.c_str(),
			sPort.empty() ? nullptr : sPort.c_str(),
			pHints ? pHints : &default_connect_hints,
			&m_rawInfo);

	if (0!=r)
	{
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
		m_nResTime = RES_OK;
		return true;
	}

	return ret_error("no suitable target service", EINVAL);
}

void CAddrInfo::Reset()
{
	if (m_rawInfo)
		evutil_freeaddrinfo(m_rawInfo);
	m_tcpAddrInfo = m_rawInfo = nullptr;
	if(m_psErrorMessage)
		delete m_psErrorMessage;
	m_nResTime = RES_ONGOING;
}

CAddrInfo::~CAddrInfo()
{
	Reset();
}

CAddrInfoPtr CAddrInfo::CachedResolve(const string & sHostname, const string &sPort, string &sErrorMsgBuf)
{
	bool dummy_run = sHostname.empty() && sPort.empty();
	auto resolve_now = [&sHostname, &sPort, &sErrorMsgBuf]() {
		auto ret = make_shared<CAddrInfo>();
		if(! ret->ResolveTcpTarget(sHostname, sPort, sErrorMsgBuf))
			ret.reset();
		return ret;
	};
	if (!cfg::dnscachetime && !dummy_run)
		return resolve_now();

	auto dnsKey = make_dns_key(sHostname, sPort);
	lockuniq lg(dnsCacheCv);

	auto expiredTime = GetTime() - cfg::dnscachetime;

	// drop old ones or at least as much as needed to store another entry
	while(dnsCleanupQ.size() >= DNS_CACHE_MAX
		|| (!dnsCleanupQ.empty() && dnsCleanupQ.front()->second->m_nResTime < expiredTime))
	{
		dnsCache.erase(dnsCleanupQ.front());
		dnsCleanupQ.pop_front();
	}
	// observe a DNS process in progress if needed
	auto cacheIt = dnsCache.find(dnsKey);
	bool resolve_here = cacheIt == dnsCache.end();

	CAddrInfoPtr ptr;
	if(resolve_here)
	{
		if(dnsCache.size() >= DNS_CACHE_MAX)
		{
			lg.unLock();
			return resolve_now();
		}
		ptr = make_shared<CAddrInfo>();
		// adding to lookup -> RESPONSIBLE FOR CLEANING NOW
		cacheIt = dnsCache.emplace(dnsKey, ptr).first;
		// run blocking resolution unlocked
		lg.unLock();
		ptr->ResolveTcpTarget(sHostname, sPort, sErrorMsgBuf);
		lg.reLock();

		dnsCacheCv.notifyAll();

		if(ptr->m_nResTime == RES_ERROR)
		{
			dnsCache.erase(cacheIt);
			// resolved here, error string was updated
			return CAddrInfoPtr();
		}

		ptr->m_nResTime = max(GetTime(), time_t(10));
		dnsCleanupQ.emplace_back(cacheIt);
		return ptr;
	}

	// -> being resolved by someone in background, get the result...
	while (ptr->m_nResTime == RES_ONGOING)
		dnsCacheCv.wait(lg);
	if (ptr->m_nResTime != RES_ERROR)
		return cacheIt->second;

	auto psCachedError = cacheIt->second->m_psErrorMessage;
	if (psCachedError)
		sErrorMsgBuf = *psCachedError;
	return CAddrInfoPtr();
}

void DropDnsCache()
{
	lockguard g(dnsCacheCv);
	dnsCleanupQ.clear();
	dnsCache.clear();
}


}
