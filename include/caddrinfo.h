#ifndef CADDRINFO_H_
#define CADDRINFO_H_

#include "meta.h"
#include "rfc2553emu.h"
#include "lockable.h"
 
#ifndef HAVE_GETADDRINFO

#warning Not sure about gai_strerror, and this define check is stupid/incorrect too
#warning but most likely it is missing too -> fake it

#ifndef gai_strerror
#define gai_strerror(x) "Generic DNS error"
#endif

#endif

class CAddrInfo;

class CAddrInfo : public lockable
{
public:
	time_t m_nExpTime;
	struct addrinfo * m_addrInfo;
	
	CAddrInfo();
	bool Resolve(const mstring & sHostname, const mstring &sPort, mstring & sErrorBuf);
	~CAddrInfo();

	typedef SHARED_PTR<CAddrInfo> SPtr;
	static SPtr CachedResolve(const mstring & sHostname, const mstring &sPort,
			mstring &sErrorMsgBuf);

	static time_t BackgroundCleanup();

protected:
	struct addrinfo * m_resolvedInfo; // getaddrinfo excrements, to cleanup

private:
	// not to be copied ever
	CAddrInfo(const CAddrInfo&);
	CAddrInfo operator=(const CAddrInfo&);

};


#endif /*CADDRINFO_H_*/
