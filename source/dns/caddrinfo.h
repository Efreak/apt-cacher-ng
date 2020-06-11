#ifndef CADDRINFO_H_
#define CADDRINFO_H_

#include "meta.h"
#include "acsmartptr.h"
#include <event2/util.h>

namespace acng
{

class ACNG_API CAddrInfo : public tLintRefcounted
{
public:
	virtual ~CAddrInfo() = default;
	virtual const evutil_addrinfo *getTcpAddrInfo() const =0;
	virtual const std::string& getError() const =0;
};

//typedef std::shared_ptr<CAddrInfo> CAddrInfoPtr;
typedef acng::lint_ptr<CAddrInfo> CAddrInfoPtr;
typedef std::function<void(CAddrInfoPtr)> tDnsResultReporter;

class ACNG_API tDnsBase
{
public:
	virtual ~tDnsBase() = default;
	// async. DNS resolution on IO thread. Reports result through the reporter.
	virtual void Resolve(cmstring & sHostname, cmstring &sPort, tDnsResultReporter) noexcept =0;
	// like above but blocking
	virtual CAddrInfoPtr Resolve(cmstring & sHostname, cmstring &sPort) =0;
};

struct tSysRes;
void AddDefaultDnsBase(tSysRes &src);

}

#endif /*CADDRINFO_H_*/
