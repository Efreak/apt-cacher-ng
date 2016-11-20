/*
 * tcpconnect.h
 *
 *  Created on: 27.02.2010
 *      Author: ed
 */

#ifndef TCPCONNECT_H_
#define TCPCONNECT_H_

#include <atomic>
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
struct fileitem;
typedef std::shared_ptr<tcpconnect> tDlStreamHandle;

class tcpconnect
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

	int m_nRcvBufSize = -1;

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

#if 0
#warning maybe simplify, return number of takes until half the buffer size was reached.
// and main loop


	// Report the size of the last chunk received from the file descriptor
	// @return An average value but only when updated, zero when not updated
	unsigned UpdateRecvBufferStats(unsigned rcvdChunkSize)
	{
		// buffer overrun? need to notify ASAP
		if(m_recvBuf.freecapa() == 0)
		{
			m_statsTemp=m_statsCursor=0;
			return m_recvBuf.freecapa();
		}
		m_statsTemp += rcvdChunkSize;
		if(0 == (++m_statsCursor & 7))
		{
			auto ret=m_statsTemp / 8;
			m_statsTemp=m_statsCursor=0;
			return ret;
		}
		return 0;
	}

#warning implement
	/*
	 * Recommended buffer object to use for reception.
	 * Size is adjusted according to socket specifics upon opening, or to default setting.
	 */
	tSS m_recvBuf;
#endif
private:

#if 0
#warning implement, either default or SO_RCVBUFSZ
	bool AdjustBufferSize();
#endif

	bool _Connect(mstring &sErrOut, int timeout);
	cfg::tRepoData::IHookHandler *m_pStateObserver=nullptr;

	unsigned m_statsCursor = 0;
	unsigned m_statsTemp = 0;

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
	virtual void RecycleIdleConnection(tDlStreamHandle & handle) =0;
	virtual tDlStreamHandle CreateConnected(cmstring &sHostname, cmstring &sPort,
				mstring &sErrOut,
				bool *pbSecondHand,
				cfg::tRepoData::IHookHandler *pStateTracker
				,bool ssl
				,int timeout
				,bool mustbevirgin
		) =0;
	virtual ~IDlConFactory() {};
};

class dl_con_factory : public IDlConFactory
{
public:
	/// Moves the connection handle to the reserve pool (resets the specified sptr).
	/// Should only be supplied with IDLE connection handles in a sane state.
	virtual void RecycleIdleConnection(tDlStreamHandle & handle) override;
	virtual tDlStreamHandle CreateConnected(cmstring &sHostname, cmstring &sPort,
				mstring &sErrOut,
				bool *pbSecondHand,
				cfg::tRepoData::IHookHandler *pStateTracker
				,bool ssl
				,int timeout
				,bool mustbevirgin
		) override;
	virtual ~dl_con_factory() {};
	void dump_status();
	time_t BackgroundCleanup();
protected:
	friend class tcpconnect;
	static std::atomic_uint g_nconns;
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
