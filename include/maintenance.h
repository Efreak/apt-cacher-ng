#ifndef MAINTENANCE_H_
#define MAINTENANCE_H_

#include "config.h"
#include "meta.h"
#include "sockio.h"
#include "acbuf.h"

#ifdef DEBUG
#define MTLOGDEBUG(x) { SendFmt << x << "\n<br>\n"; }
#define MTLOGASSERT(x, y) {if(!(x)) SendFmt << "<div class=\"ERROR\">" << y << "</div>\n<br>\n";}
//#define MTLOGVERIFY(x, y) MTLOGASSERT(x, y)
#else
#define MTLOGASSERT(x, y) {}
#define MTLOGDEBUG(x) {}
//#define MTLOGVERIFY(x, y) x
#endif

#define MAINT_PFX "maint_"

class tSpecialRequest
{
public:
	enum eMaintWorkType : int
	{
		workNotSpecial =0,

		// expiration types
		workExExpire,
		workExList,
		workExPurge,
		workExListDamaged,
		workExPurgeDamaged,
		workExTruncDamaged,
		//workBGTEST,
		workUSERINFO,
		workMAINTREPORT,
		workAUTHREQUEST,
		workAUTHREJECT,
		workIMPORT,
		workMIRROR,
		workDELETE,
		workDELETECONFIRM,
		workCOUNTSTATS,
		workSTYLESHEET,
		workTraceStart,
		workTraceEnd
	};
	/*!
	 *  @brief Main execution method for maintenance tasks.
	 */
	virtual void Run(const mstring &cmd)=0;

	tSpecialRequest(int fd, tSpecialRequest::eMaintWorkType type);
	virtual ~tSpecialRequest();

protected:
	inline void SendChunk(const mstring &x) { SendChunk(x.data(), x.size()); }

	inline void SendChunk(const tSS &x, bool b=false){ SendChunk(x.data(), x.length(), b); }
	void SendChunk(const char *data, size_t size, bool bRemoteOnly=false);
	inline void SendChunk(const char *x, bool b=false) { SendChunk(x, x?strlen(x):0, b); }
	// for customization in base classes
	virtual void AfterSendChunk(const char* /*data*/, size_t /*size*/) {};

	bool SendRawData(const char *data, size_t len, int flags);
	virtual void EndTransfer();
	mstring & GetHostname();
	//void SendDecoration(bool bBegin, const char *szDecoFile=NULL);
	void SendChunkedPageHeader(const char *httpcode=NULL, const char *mimetype=NULL);
	int m_reportFD;
	LPCSTR m_szDecoFile;
	LPCSTR GetTaskName();
private:
	tSpecialRequest(const tSpecialRequest&);
	tSpecialRequest& operator=(const tSpecialRequest&);
	mstring m_sHostname;

public:
	// dirty little RAII helper to send data after formating it, uses a shared buffer presented
	// to the user via macro
	class tFmtSendObj
	{
	public:
		inline tFmtSendObj(tSpecialRequest *p, bool remoteOnly)
		: m_parent(*p), m_bRemoteOnly(remoteOnly) { }
		inline ~tFmtSendObj()
		{
			if (!m_parent.m_fmtHelper.empty())
			{
				m_parent.SendChunk(m_parent.m_fmtHelper, m_bRemoteOnly);
				m_parent.m_fmtHelper.clear();
			}
		}
		tSpecialRequest &m_parent;
	private:
		tFmtSendObj operator=(const tSpecialRequest::tFmtSendObj&);
		bool m_bRemoteOnly;
	};

#define SendFmt tFmtSendObj(this, false).m_parent.m_fmtHelper
#define SendFmtRemote tFmtSendObj(this, true).m_parent.m_fmtHelper

	tSS m_fmtHelper;

	static eMaintWorkType DispatchMaintWork(cmstring &cmd, const char *auth);
	static void RunMaintWork(eMaintWorkType jobType, cmstring &cmd, int fd);

protected:
	eMaintWorkType m_mode;

	static tSpecialRequest* MakeMaintWorker(int fd, eMaintWorkType type);
};

#endif /*MAINTENANCE_H_*/
