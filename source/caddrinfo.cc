
#include "meta.h"
#include "caddrinfo.h"
#include "sockio.h"
#include "acfg.h"
#include "debug.h"
#include "lockable.h"
#include "cleaner.h"

#include <queue>

using namespace std;

namespace acng
{
static const unsigned DNS_CACHE_MAX=255;

base_with_condition dnsCacheCv;
map<tStrPair,CAddrInfoPtr> dnsCache;

// simple prio-queue to track the oldest entries
typedef decltype(dnsCache)::iterator dnsIter;
struct tDnsCmpEarliest
{
	bool operator() (const dnsIter& x,const dnsIter& y)
	{
		return (x->second->m_nExpTime > y->second->m_nExpTime);
	}
};
priority_queue<dnsIter, vector<dnsIter>, tDnsCmpEarliest> dnsCleanupQ;

bool CAddrInfo::Resolve(const string & sHostname, const string &sPort,
		string & sErrorBuf)
{
	LOGSTART2("CAddrInfo::Resolve", "Resolving " << sHostname);

	m_bError = false;
	m_bResInProgress = true;

	LPCSTR port = sPort.empty() ? nullptr : sPort.c_str();

	static evutil_addrinfo hints =
	{
	// we provide numbers, no resolution needed; only supported addresses
			(port ? AI_NUMERICSERV:0) | AI_ADDRCONFIG,
			PF_UNSPEC, SOCK_STREAM, IPPROTO_TCP,
			0, nullptr, nullptr, nullptr };

	// if only one family is specified, filter on this earlier
	if(cfg::conprotos[0] != PF_UNSPEC && cfg::conprotos[1] == PF_UNSPEC)
		hints.ai_family = cfg::conprotos[0];

	if (m_resolvedInfo)
	{
		freeaddrinfo(m_resolvedInfo);
		m_resolvedInfo=nullptr;
	}

	int r=evutil_getaddrinfo(sHostname.c_str(), port, &hints, &m_resolvedInfo);

	if (0!=r)
	{
		sErrorBuf=(tSS()<<"503 DNS error for hostname "<<sHostname<<": "<<gai_strerror(r)
				<<". If "<<sHostname<<" refers to a configured cache repository, "
				"please check the corresponding configuration file.");
		m_bError = true;
		m_bResInProgress = false;
		return false;
	}

	LOG("Host resolved");
	
	// find any suitable-looking entry and keep a pointer to it
	for (auto pCur=m_resolvedInfo; pCur && m_bResInProgress ; pCur = pCur->ai_next)
	{
		if (pCur->ai_socktype == SOCK_STREAM&& pCur->ai_protocol == IPPROTO_TCP)
		{
			m_addrInfo=pCur;
			m_bResInProgress = false;
			return true;
		}
	}

	if(!m_bResInProgress)
		return true;

	LOG("couldn't find working DNS config");
	m_bError = true;
	m_bResInProgress = false;
	sErrorBuf="500 DNS resolution error";
	return false;
}

CAddrInfo::~CAddrInfo()
{
	if (m_resolvedInfo)
		freeaddrinfo(m_resolvedInfo);
}

CAddrInfoPtr CAddrInfo::CachedResolve(const string & sHostname, const string &sPort, string &sErrorMsgBuf)
{
	// set if someone is responsible for cleaning it up later
	bool added2cache = false;
	auto dnsKey=make_pair(sHostname, sPort);

	CAddrInfoPtr job;

	if (!cfg::dnscachetime)
	{
		job.reset(new CAddrInfo);
		goto plain_resolve;
	}

	if(!cfg::dnscachetime)
		goto plain_resolve;

	{
		lockuniq lg(dnsCacheCv);

		auto now = GetTime();

		// every caller does little cleanup work first
		while(!dnsCleanupQ.empty())
		{
			auto p = dnsCleanupQ.top()->second;
			if(p->m_nExpTime >= now)
			{
				dnsCache.erase(dnsCleanupQ.top());
				dnsCleanupQ.pop();
			}
			else if(dnsCleanupQ.size() < DNS_CACHE_MAX) // remaining ones are fresh enough unless we need to shrink the cache
				break;
		}

		// observe a DNS process in progress if needed
		auto it = dnsCache.find(dnsKey);
		if(it != dnsCache.end())
		{
			auto p = it->second;
			while(p->m_bResInProgress)
				dnsCacheCv.wait(lg);
			if(p->m_bError)
			{
				sErrorMsgBuf = string("Error resolving ") + sHostname;
				return job; // still invalid pointer
			}
			// otherwise it's ok
			return p;
		}
		job = make_shared<CAddrInfo>();
		added2cache = dnsCache.size() < DNS_CACHE_MAX; // if looks like DOS, don't dirty the temp queue with it
		if(added2cache)
			dnsCache.insert(make_pair(dnsKey, job));
	}
	plain_resolve:

	bool ok = job->Resolve(sHostname, sPort, sErrorMsgBuf);

	// there is a slight risk that someone else expired our entry in the meantime, but it's unlikely and should be harmless
	if(added2cache)
	{
		lockguard lg(dnsCacheCv);
		dnsCacheCv.notifyAll();
		if(ok)
		{
			auto it = dnsCache.find(dnsKey);
			if(it != dnsCache.end())
				dnsCleanupQ.push(it);
		}
		else
			dnsCache.erase(dnsKey);
	}
	return ok ? job : CAddrInfoPtr();
}

void DropDnsCache()
{
	lockguard g(dnsCacheCv);
	dnsCache.clear();
	dnsCleanupQ = decltype(dnsCleanupQ)();
}

}
