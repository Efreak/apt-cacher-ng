/*
 * tcpconnect.h
 *
 *  Created on: 27.02.2010
 *      Author: ed
 */

#ifndef TCPCONNECT_H_
#define TCPCONNECT_H_

#include <memory>
#include "meta.h"
#include "sockio.h"
#include "acfg.h"
#include <memory>

#ifdef HAVE_SSL
#include <openssl/bio.h>
#include "acbuf.h"
#endif

namespace acng
{

class tcpconnect;
class fileitem;
typedef std::shared_ptr<tcpconnect> tDlStreamHandle;

class ACNG_API tcpconnect
{
public:
	virtual ~tcpconnect();

	virtual int GetFD() { return m_conFd; }
	inline cmstring & GetHostname() { return m_sHostName; }
	inline cmstring & GetPort() { return m_sPort; }
	void Disconnect();

#ifdef HAVE_SSL
	inline BIO* GetBIO() { return m_bio;};
#endif

protected:
	tcpconnect operator=(const tcpconnect&);
	tcpconnect(const tcpconnect&) =default;
	tcpconnect(cfg::tRepoData::IHookHandler *pStateReport);

	int m_conFd =-1;
	mstring m_sHostName, m_sPort;

	std::weak_ptr<fileitem> m_lastFile;

public:
	//! @brief Remember the file name belonging to the recently initiated transfer
	inline void KnowLastFile(WEAK_PTR<fileitem> spRemItem) { m_lastFile = spRemItem; }
	//! @brief Invalidate (truncate) recently touched file
	void KillLastFile();
	//! @brief Request tunneling with CONNECT and change identity if succeeded, and start TLS
	bool StartTunnel(const tHttpUrl & realTarget, mstring& sError, cmstring *psAuthorization, bool bDoSSLinit);

private:
	std::string _Connect(int timeout);
	cfg::tRepoData::IHookHandler *m_pStateObserver=nullptr;

protected:
#ifdef HAVE_SSL
	BIO *m_bio = nullptr;
	SSL_CTX * m_ctx = nullptr;
	SSL * m_ssl = nullptr;
	bool SSLinit(mstring &sErr, cmstring &host, cmstring &port);
#endif

	friend class dl_con_factory;
};

class IDlConFactory
{
public:
	/// Moves the connection handle to the reserve pool (resets the specified sptr).
	/// Should only be supplied with IDLE connection handles in a sane state.
	virtual void RecycleIdleConnection(tDlStreamHandle & handle) const =0;
	virtual tDlStreamHandle CreateConnected(cmstring &sHostname, cmstring &sPort,
				mstring &sErrOut,
				bool *pbSecondHand,
				cfg::tRepoData::IHookHandler *pStateTracker
				,bool ssl
				,int timeout
				,bool mustbevirgin
		) const =0;
	virtual ~IDlConFactory() {};
};

class ACNG_API dl_con_factory : public IDlConFactory
{
public:
	/// Moves the connection handle to the reserve pool (resets the specified sptr).
	/// Should only be supplied with IDLE connection handles in a sane state.
	virtual void RecycleIdleConnection(tDlStreamHandle & handle) const override;
	virtual tDlStreamHandle CreateConnected(cmstring &sHostname, cmstring &sPort,
				mstring &sErrOut,
				bool *pbSecondHand,
				cfg::tRepoData::IHookHandler *pStateTracker
				,bool ssl
				,int timeout
				,bool mustbevirgin
		) const override;
	virtual ~dl_con_factory() {};
	void dump_status();
	time_t BackgroundCleanup();
protected:
	friend class tcpconnect;
};

extern dl_con_factory g_tcp_con_factory;

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
}

#endif /* TCPCONNECT_H_ */
