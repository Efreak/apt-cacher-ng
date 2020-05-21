#ifndef CADDRINFO_H_
#define CADDRINFO_H_

#include "meta.h"
#include <event2/util.h>

namespace acng
{
struct tSysRes;

class ACNG_API CAddrInfo
{
public:
	virtual ~CAddrInfo();
	const evutil_addrinfo *getTcpAddrInfo() const;
	const std::string& getError() const;
};

typedef std::shared_ptr<CAddrInfo> CAddrInfoPtr;
typedef std::function<void(CAddrInfoPtr)> tDnsResultReporter;

class tDnsBase
{
public:
	virtual ~tDnsBase();
	// async. DNS resolution on IO thread. Reports result through the reporter.
	void Resolve(cmstring & sHostname, cmstring &sPort, tDnsResultReporter) noexcept;
	// like above but blocking
	std::shared_ptr<CAddrInfo> Resolve(cmstring & sHostname, cmstring &sPort);
};
}

#endif /*CADDRINFO_H_*/
