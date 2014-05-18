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

class tWUIPage
{
public:
	/*!
	 *  @brief Main execution method for maintenance tasks.
	 */
	virtual void Run(const mstring &cmd)=0;

	tWUIPage(int fd);
	virtual ~tWUIPage();

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
	const char *m_szDecoFile;
private:
	tWUIPage(const tWUIPage&);
	tWUIPage& operator=(const tWUIPage&);
	mstring m_sHostname;

public:
	// dirty little RAII helper to send data after formating it, uses a shared buffer presented
	// to the user via macro
	class tFmtSendObj
	{
	public:
		inline tFmtSendObj(tWUIPage *p, bool remoteOnly)
		: m_parent(*p), m_bRemoteOnly(remoteOnly) { }
		inline ~tFmtSendObj()
		{
			if (!m_parent.m_fmtHelper.empty())
			{
				m_parent.SendChunk(m_parent.m_fmtHelper, m_bRemoteOnly);
				m_parent.m_fmtHelper.clear();
			}
		}
		tWUIPage &m_parent;
	private:
		tFmtSendObj operator=(const tWUIPage::tFmtSendObj&);
		bool m_bRemoteOnly;
	};

#define SendFmt tFmtSendObj(this, false).m_parent.m_fmtHelper
#define SendFmtRemote tFmtSendObj(this, true).m_parent.m_fmtHelper

	tSS m_fmtHelper;
};

void DispatchAndRunMaintTask(cmstring &cmd, int fd, const char *auth);

#endif /*MAINTENANCE_H_*/
