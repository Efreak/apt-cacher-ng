/*
 * tcpconnect.h
 *
 *  Created on: 27.02.2010
 *      Author: ed
 */

#ifndef TCPCONNECT_H_
#define TCPCONNECT_H_

#include <atomic>

#include "meta.h"
#include "sockio.h"

#include <memory>

#ifdef HAVE_SSL
#include <openssl/bio.h>
#include "acbuf.h"
#endif

class tcpconnect;
class fileitem;
typedef std::shared_ptr<tcpconnect> tTcpHandlePtr;

class tcpconnect
{
public:
	virtual ~tcpconnect();

	static SHARED_PTR<tcpconnect> CreateConnected(cmstring &sHostname, cmstring &sPort,
			mstring &sErrOut,
			bool *pbSecondHand,
			acfg::tRepoData::IHookHandler *pStateTracker
			,bool ssl
			,int timeout
			,bool mustbevirgin
	);

	/// Moves the connection handle to the reserve pool (resets the specified sptr).
	/// Should only be supplied with IDLE connection handles in a sane state.
	static void RecycleIdleConnection(tTcpHandlePtr & handle);
	static time_t BackgroundCleanup();

	inline int GetFD() { return m_conFd; }
	inline cmstring & GetHostname() { return m_sHostName; }
	inline cmstring & GetPort() { return m_sPort; }
	void Disconnect();

	static void dump_status();

#ifdef HAVE_SSL
	inline BIO* GetBIO() { return m_bio;};
#endif

protected:
	tcpconnect operator=(const tcpconnect&);
	tcpconnect(const tcpconnect&) =default;
	tcpconnect(acfg::tRepoData::IHookHandler *pStateReport);

	int m_conFd =-1;
	mstring m_sHostName, m_sPort;

	std::weak_ptr<fileitem> m_lastFile;

	static std::atomic_uint g_nconns;

public:
	//! @brief Remember the file name belonging to the recently initiated transfer
	inline void KnowLastFile(WEAK_PTR<fileitem> spRemItem) { m_lastFile = spRemItem; }
	//! @brief Invalidate (truncate) recently touched file
	void KillLastFile();
	//! @brief Request tunneling with CONNECT and change identity if succeeded, and start TLS
	bool StartTLStunnel(const tHttpUrl & realTarget, mstring& sError, cmstring *psAuthorization=nullptr);

private:
	bool _Connect(mstring &sErrOut, int timeout);
	acfg::tRepoData::IHookHandler *m_pStateObserver=nullptr;

#ifdef HAVE_SSL
	BIO *m_bio = nullptr;
	SSL_CTX * m_ctx = nullptr;
	SSL * m_ssl = nullptr;
	bool SSLinit(mstring &sErr, cmstring &host, cmstring &port);
#endif
};
/*
// little tool for related classes, helps counting all object instances
class instcount
{
	private:
	typedef std::atomic_uint tInstCounter;
	tInstCounter& m_instCount;

public:
	inline instcount(tInstCounter& cter) : m_instCount(cter) {
		m_instCount.fetch_add(1);
	}
	virtual ~instcount() {
		m_instCount.fetch_add(-1);
	}
	unsigned int GetInstCount(unsigned type) { return m_instCount.load();}
};
*/

#endif /* TCPCONNECT_H_ */
