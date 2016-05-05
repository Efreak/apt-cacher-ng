
#include "meta.h"
#include "caddrinfo.h"
#include "sockio.h"
#include "acfg.h"
#include "debug.h"
#include "lockable.h"
#include "cleaner.h"

#include <unordered_map>

using namespace std;

static class : public unordered_map<string, CAddrInfo::SPtr>, public base_with_mutex {} mapDnsCache;

bool CAddrInfo::Resolve(const string & sHostname, const string &sPort,
		string & sErrorBuf)
{
	LOGSTART2("CAddrInfo::Resolve", "Resolving " << sHostname);

	LPCSTR port = sPort.empty() ? nullptr : sPort.c_str();

	static struct addrinfo hints =
	{
	// we provide numbers, no resolution needed; only supported addresses
			(port ? AI_NUMERICSERV:0) | AI_ADDRCONFIG,
			PF_UNSPEC, SOCK_STREAM, IPPROTO_TCP,
			0, nullptr, nullptr, nullptr };

	// if only one family is specified, filter on this earlier
	if(acfg::conprotos[0] != PF_UNSPEC && acfg::conprotos[1] == PF_UNSPEC)
		hints.ai_family = acfg::conprotos[0];

	if (m_resolvedInfo)
	{
		freeaddrinfo(m_resolvedInfo);
		m_resolvedInfo=nullptr;
	}

	int r=getaddrinfo(sHostname.c_str(), port, &hints, &m_resolvedInfo);

	if (0!=r)
	{
		sErrorBuf=(tSS()<<"503 DNS error for hostname "<<sHostname<<": "<<gai_strerror(r)
				<<". If "<<sHostname<<" refers to a configured cache repository, "
				"please check the corresponding configuration file.");
		return false;
	}

	LOG("Host resolved");

	for (auto pCur=m_resolvedInfo; pCur; pCur = pCur->ai_next)
	{
		if (pCur->ai_socktype == SOCK_STREAM&& pCur->ai_protocol == IPPROTO_TCP)
		{
			m_addrInfo=pCur;
			return true;
		}
	}
	LOG("couldn't find working DNS config");

	sErrorBuf="500 DNS resolution error";

	// TODO: remove me from the map
	return false;
}

CAddrInfo::~CAddrInfo()
{
	if (m_resolvedInfo)
		freeaddrinfo(m_resolvedInfo);
}
	

CAddrInfo::SPtr CAddrInfo::CachedResolve(const string & sHostname, const string &sPort, string &sErrorMsgBuf)
{
	//time_t timeExpired=time(nullptr)+acfg::dnscachetime;
	time_t now(time(0));
	mstring dnsKey=sHostname+":"+sPort;

	SPtr p;
	{
		lockguard g(mapDnsCache);
		SPtr localEntry;
		SPtr & cand = acfg::dnscachetime>0 ? mapDnsCache[dnsKey] : localEntry;
		if(cand && cand->m_nExpTime >= now)
			return cand;
		cand.reset(new CAddrInfo);
		p=cand;
		// lock the internal class and keep it until we are done with preparations
		p->m_obj_mutex.lock();
	}

	if(!p)
		return SPtr(); // weird...

	if (p->Resolve(sHostname, sPort, sErrorMsgBuf))
	{
		p->m_nExpTime = time(0) + acfg::dnscachetime;
#ifndef MINIBUILD
		g_victor.ScheduleFor(p->m_nExpTime, cleaner::TYPE_EXDNS);
#endif
		p->m_obj_mutex.unlock();
	}
	else // not good, remove from the cache again
	{
		aclog::err( (tSS()<<"Error resolving "<<dnsKey<<": " <<sErrorMsgBuf).c_str());
		lockguard g(mapDnsCache);
		mapDnsCache.erase(dnsKey);
		p->m_obj_mutex.unlock();
		p.reset();
	}

	return p;
}

time_t CAddrInfo::BackgroundCleanup()
{
	lockguard g(mapDnsCache);
	time_t now(GetTime()), ret(END_OF_TIME);

	for(auto it=mapDnsCache.begin(); it!=mapDnsCache.end(); )
	{
		if(it->second)
		{
			if(it->second->m_nExpTime<=now)
			{
				mapDnsCache.erase(it++);
				continue;
			}
			else
				ret=min(ret, it->second->m_nExpTime);
		}

		++it;
	}
	return ret;
}

void DropDnsCache()
{
	lockguard g(mapDnsCache);
	mapDnsCache.clear();
}
