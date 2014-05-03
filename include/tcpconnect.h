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

#ifdef HAVE_SSL
#include <openssl/bio.h>
#include "acbuf.h"
#endif

class tcpconnect;
class fileitem;
typedef SHARED_PTR<tcpconnect> tTcpHandlePtr;

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
	);

	/// Moves the connection handle to the reserve pool (resets the specified sptr).
	/// Shold only be supplied with IDLE connection handles in a sane state.
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
	tcpconnect(const tcpconnect&);
	tcpconnect();

	int m_conFd;
	mstring m_sHostName, m_sPort;

	acfg::tRepoData::IHookHandler *m_pConnStateObserver;

	WEAK_PTR<fileitem> m_lastFile;

public:
	//! @brief Remember the file name belonging to the recently initiated transfer
	inline void KnowLastFile(WEAK_PTR<fileitem> spRemItem) { m_lastFile = spRemItem; }
	//! @brief Invalidate (truncate) recently touched file
	void KillLastFile();

private:
	bool Connect(mstring &sErrOut, int timeout);

#ifdef HAVE_SSL
	BIO *m_bio;
	SSL_CTX * m_ctx;
	SSL * m_ssl;
	bool SSLinit(mstring &sErr, cmstring &host, cmstring &port);
#endif
};


#endif /* TCPCONNECT_H_ */
