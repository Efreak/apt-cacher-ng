#ifndef MAINTENANCE_H_
#define MAINTENANCE_H_

#include "config.h"
#include "meta.h"
#include "sockio.h"
#include "acbuf.h"

static const std::string sBRLF("<br>\n");

#ifdef DEBUG
#define MTLOGDEBUG(x) { SendFmt << x << sBRLF; }
#define MTLOGASSERT(x, y) {if(!(x)) SendFmt << "<div class=\"ERROR\">" << y << "</div>\n" << sBRLF;}
//#define MTLOGVERIFY(x, y) MTLOGASSERT(x, y)
#else
#define MTLOGASSERT(x, y) {}
#define MTLOGDEBUG(x) {}
//#define MTLOGVERIFY(x, y) x
#endif

#define MAINT_PFX "maint_"

namespace acng
{
class ACNG_API tSpecialRequest
{
public:
	enum eMaintWorkType : char
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
		workTraceEnd,
//		workJStats, // disabled, probably useless
		workTRUNCATE,
		workTRUNCATECONFIRM
	};
	struct tRunParms
	{
		int fd;
		tSpecialRequest::eMaintWorkType type;
		cmstring cmd;
	};
	/*!
	 *  @brief Main execution method for maintenance tasks.
	 */
	virtual void Run() =0;

	tSpecialRequest(const tRunParms& parms);
	virtual ~tSpecialRequest();

protected:
	inline void SendChunk(const mstring &x) { SendChunk(x.data(), x.size()); }

	void SendChunk(const char *data, size_t size);
	void SendChunkRemoteOnly(const char *data, size_t size);
	inline void SendChunk(const char *x) { SendChunk(x, x?strlen(x):0); }
	inline void SendChunk(const tSS &x){ SendChunk(x.data(), x.length()); }
	// for customization in base classes
	virtual void SendChunkLocalOnly(const char* /*data*/, size_t /*size*/) {};

	bool SendRawData(const char *data, size_t len, int flags);

	cmstring & GetMyHostPort();
	void SendChunkedPageHeader(const char *httpstatus, const char *mimetype);
	LPCSTR m_szDecoFile = nullptr;
	LPCSTR GetTaskName();
	tRunParms m_parms;

private:
	tSpecialRequest(const tSpecialRequest&);
	tSpecialRequest& operator=(const tSpecialRequest&);
	mstring m_sHostPort;
	bool m_bChunkHeaderSent=false;

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
				if(m_bRemoteOnly)
					m_parent.SendChunkRemoteOnly(m_parent.m_fmtHelper.data(), m_parent.m_fmtHelper.size());
				else
					m_parent.SendChunk(m_parent.m_fmtHelper);
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
#define SendChunkSZ(x) SendChunk(WITHLEN(x))

	tSS m_fmtHelper;

	static eMaintWorkType DispatchMaintWork(cmstring &cmd, const char *auth);
	static void RunMaintWork(eMaintWorkType jobType, cmstring& cmd, int fd);

protected:
	static tSpecialRequest* MakeMaintWorker(const tRunParms& parms);
};

std::string to_base36(unsigned int val);
static const string relKey("/Release"), inRelKey("/InRelease");
static cmstring sfxXzBz2GzLzma[] = { ".xz", ".bz2", ".gz", ".lzma"};
static cmstring sfxXzBz2GzLzmaNone[] = { ".xz", ".bz2", ".gz", ".lzma", ""};
static cmstring sfxMiscRelated[] = { "", ".xz", ".bz2", ".gz", ".lzma", ".gpg", ".diff/Index"};
}

#endif /*MAINTENANCE_H_*/
