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
public:
	bool m_bError = false; // to pass the error hint to waiting resolver
	bool m_bResInProgress = false; // to be just in the end when the resolution is finished

	time_t m_nExpTime=0;

	// hint to the first descriptor of any TCP type
	evutil_addrinfo * m_addrInfo=nullptr;
	
	CAddrInfo() =default;
	bool Resolve(const mstring & sHostname, const mstring &sPort, mstring & sErrorBuf);
	~CAddrInfo();

	static SHARED_PTR<CAddrInfo> CachedResolve(const mstring & sHostname, const mstring &sPort,
			mstring &sErrorMsgBuf);

protected:
	evutil_addrinfo * m_resolvedInfo=nullptr; // getaddrinfo excrements, to cleanup
private:
	// not to be copied ever
	CAddrInfo(const CAddrInfo&) = delete;
	CAddrInfo operator=(const CAddrInfo&) = delete;

};

typedef SHARED_PTR<CAddrInfo> CAddrInfoPtr;

}

#endif /*CADDRINFO_H_*/
