#ifndef CADDRINFO_H_
#define CADDRINFO_H_

#include "meta.h"
#include "lockable.h"

#include <event2/util.h>
#include "sockio.h"

namespace acng
{

class CAddrInfo
{
	// not to be copied ever
	CAddrInfo(const CAddrInfo&) = delete;
	CAddrInfo operator=(const CAddrInfo&) = delete;

	// special values of creation time
	static const time_t RES_ONGOING = 0;
	static const time_t RES_ERROR = 1;
	// special flag, to be replaced by creation time where possible
	static const time_t RES_OK = 2;

	evutil_addrinfo * m_resolvedInfo = nullptr; // raw returned data from getaddrinfo
	time_t m_nResTime = RES_ONGOING;

	std::string* m_psErrorMessage = nullptr;

public:
	// hint to the first descriptor of any TCP type
	evutil_addrinfo * m_addrInfo=nullptr;
	
	CAddrInfo() = default;
	// blocking DNS resolution. Supposed to be called only once!
	bool Resolve(const mstring & sHostname, const mstring &sPort, mstring & sErrorBuf);
	~CAddrInfo();

	static SHARED_PTR<CAddrInfo> CachedResolve(const mstring & sHostname, const mstring &sPort,
			mstring &sErrorMsgBuf);
};

typedef SHARED_PTR<CAddrInfo> CAddrInfoPtr;

}

#endif /*CADDRINFO_H_*/
