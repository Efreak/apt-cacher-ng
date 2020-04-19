#ifndef CADDRINFO_H_
#define CADDRINFO_H_

#include "meta.h"
#include "lockable.h"

#include <event2/util.h>
#include "sockio.h"

namespace acng
{

class CAddrInfo : public base_with_mutex
{
	// not to be copied ever
	CAddrInfo(const CAddrInfo&) = delete;
	CAddrInfo operator=(const CAddrInfo&) = delete;

	// some hints for cached entries
	std::string m_sError;
	time_t m_expTime = END_OF_TIME;

	// raw returned data from getaddrinfo
	evutil_addrinfo * m_rawInfo = nullptr;
	// shortcut for iterators, first in the list with TCP target
	evutil_addrinfo * m_tcpAddrInfo = nullptr;

public:
	
	CAddrInfo() = default;
	/**
	 *  blocking DNS resolution. Supposed to be called only once.
	 *  Will result either in the node containing m_tcpAddrInfo (on success) or m_sError
	 *  set to non-empty.
	 *
	 *  Can also be used with an empty hostname, to resolve just by port.
	 */
	void ResolveTcpTarget(const mstring & sHostname, const mstring &sPort,
			const evutil_addrinfo* hints,
			bool & bTransientError);

	void Reset();

	~CAddrInfo();

	static SHARED_PTR<CAddrInfo> CachedResolve(const mstring & sHostname, const mstring &sPort);

	//tDnsIterator getIterator(int pf_filter) const { return tDnsIterator(pf_filter, this); }
	/**
	 * Return a pre-located pointer which points on the first TCP compatible address or nullptr
	 * if no such found.
	 */
	const evutil_addrinfo *getTcpAddrInfo() const { return m_tcpAddrInfo; }

	bool HasResult() { return m_tcpAddrInfo || !m_sError.empty(); }
	bool HasError() { return !m_tcpAddrInfo; }
	time_t GetExpirationTime() { return m_expTime; }
	const std::string& GetError() {
		return m_sError;
	}
};

typedef SHARED_PTR<CAddrInfo> CAddrInfoPtr;


}

#endif /*CADDRINFO_H_*/
