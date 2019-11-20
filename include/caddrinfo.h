#ifndef CADDRINFO_H_
#define CADDRINFO_H_

#include "meta.h"
#include "lockable.h"

#include <event2/util.h>
#include "sockio.h"

namespace acng
{

class ACNG_API CAddrInfo
{
	// not to be copied ever
	CAddrInfo(const CAddrInfo&) = delete;
	CAddrInfo operator=(const CAddrInfo&) = delete;

	// resolution results (or hint for caching)
	std::string m_sError;
	time_t m_expTime = MAX_VAL(time_t);

	// C-style callback for libevent
	static void cb_dns(int result, struct evutil_addrinfo *results, void *arg);
	static void cb_invoke_dns_res(int result, short what, void *arg);

	// raw returned data from getaddrinfo
	evutil_addrinfo * m_rawInfo = nullptr;
	// shortcut for iterators, first in the list with TCP target
	evutil_addrinfo * m_tcpAddrInfo = nullptr;

	CAddrInfo() = default;

	void Reset();
	static void clean_dns_cache();

public:
	typedef std::function<void(SHARED_PTR<CAddrInfo>)> tDnsResultReporter;

	~CAddrInfo();

	// async. DNS resolution on IO thread. Reports result through the reporter.
	static void Resolve(cmstring & sHostname, cmstring &sPort, tDnsResultReporter);
	// like above but blocking resolution
	static SHARED_PTR<CAddrInfo> Resolve(cmstring & sHostname, cmstring &sPort);

	const evutil_addrinfo *getTcpAddrInfo() const { return m_tcpAddrInfo; }
	const std::string& getError() const { return m_sError; }

	// iih, just for building of a special element regardsless of private ctor
	static SHARED_PTR<CAddrInfo> make_fatal_failure_hint();
};

typedef SHARED_PTR<CAddrInfo> CAddrInfoPtr;


}

#endif /*CADDRINFO_H_*/
