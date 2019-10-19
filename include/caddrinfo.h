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

	time_t m_nResTime = RES_ONGOING;

	std::string* m_psErrorMessage = nullptr;

	// raw returned data from getaddrinfo
	evutil_addrinfo * m_rawInfo = nullptr;
	// shortcut for iterators, first in the list with TCP target
	evutil_addrinfo * m_tcpAddrInfo = nullptr;

public:
	
	CAddrInfo() = default;
	// blocking DNS resolution. Supposed to be called only once!
	bool ResolveTcpTarget(const mstring & sHostname, const mstring &sPort, mstring & sErrorBuf);

	// methods for internal and reusable operations
	int ResolveRaw(const char *hostname, const mstring &sPort, const evutil_addrinfo* hints);
	void Reset();

	~CAddrInfo();

	static SHARED_PTR<CAddrInfo> CachedResolve(const mstring & sHostname, const mstring &sPort,
			mstring &sErrorMsgBuf);

	//tDnsIterator getIterator(int pf_filter) const { return tDnsIterator(pf_filter, this); }
	/**
	 * Return a pre-located pointer which points on the first TCP compatible address or nullptr
	 * if no such found.
	 */
	const evutil_addrinfo *getTcpAddrInfo() const { return m_tcpAddrInfo; }
};

typedef SHARED_PTR<CAddrInfo> CAddrInfoPtr;


}

#endif /*CADDRINFO_H_*/
