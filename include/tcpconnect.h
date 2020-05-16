/*
 * tcpconnect.h
 *
 *  Created on: 27.02.2010
 *      Author: ed
 */

#ifndef TCPCONNECT_H_
#define TCPCONNECT_H_

#include "meta.h"
#include "sockio.h"
#include "acfg.h"
#include "confactory.h"
#include <unordered_map>

#ifdef HAVE_SSL
#include <openssl/bio.h>
#include "acbuf.h"
#endif

namespace acng
{
class tDnsBase;
class fileitem;
using tConnectionCache = std::unordered_multimap<std::string, tDlStreamHandle>;

// XXX: this is actually not a really clean abstraction, the "tcpconnect" class is used
// as plain base class also for UDS. OTOH the overhead is low, not worth restructuring
// everything and dragging larger vtables around.

class ACNG_API tcpconnect
{
public:
	virtual ~tcpconnect();
	tcpconnect(tSysRes& res, cmstring& sHost, cmstring& sPort, cfg::IHookHandler *pStateReport);

	virtual evutil_socket_t GetFD() const { return m_conFd; }
	inline cmstring & GetHostname() const { return m_sHostName; }
	inline cmstring & GetPort() const { return m_sPort; }

#ifdef HAVE_SSL
	inline BIO* GetBIO() const { return m_bio;};
#endif

protected:
	tcpconnect operator=(const tcpconnect&);
	tcpconnect(const tcpconnect&) =default;

	tSysRes& m_sysres;
	evutil_socket_t m_conFd = -1;
	// this comes probably for free, 2xint32 fits into 64bit word
	evutil_socket_t m_bUseSsl = (evutil_socket_t) false;
	mstring m_sHostName, m_sPort;
	cfg::IHookHandler *m_pStateObserver=nullptr;

	std::weak_ptr<fileitem> m_lastFile;

	// create a plain TCP connection
	static void DoConnect(cmstring& sHost, cmstring& sPort, cfg::IHookHandler *pStateReport,
			int timeout,
			IDlConFactory::funcRetCreated resRep, tSysRes& dnsBase);

	void DoSslSetup(bool doHttpUpgradeFirst);
	bool IsSslMode() { return m_bUseSsl; }

	void Disconnect();

	// the actual stack/state information for the connection progress
	struct tConnProgress;
	friend struct tConnProgress;

public:
	//! @brief Remember the file name belonging to the recently initiated transfer
	inline void KnowLastFile(WEAK_PTR<fileitem> spRemItem) { m_lastFile = spRemItem; }
	//! @brief Invalidate (truncate) recently touched file
	void KillLastFile();
	//! @brief Request tunneling with CONNECT and change identity if succeeded, and start TLS
	mstring StartTunnel(const tHttpUrl & realTarget, cmstring *psAuthorization, bool bDoSSLinit);

protected:
#ifdef HAVE_SSL
	SSL *m_ssl = nullptr;
	BIO *m_bio = nullptr;
	mstring SSLinit(cmstring &host, cmstring &port);
#endif

	// intrusive storage of some cache parameters
	// it's a trade-off: those fields are likely to be used multiple times so this saves the allocation of another control block
	friend class dl_con_factory;
	tConnectionCache::iterator m_cacheIt;
	event *m_pCacheCheckEvent = nullptr;
};
}

#endif /* TCPCONNECT_H_ */
