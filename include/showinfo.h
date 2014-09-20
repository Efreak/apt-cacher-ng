#ifndef SHOWINFO_H_
#define SHOWINFO_H_

#include "maintenance.h"
#include <list>

class tMarkupFileSend : public tSpecialRequest
{
public:
	virtual ~tMarkupFileSend() {};
	void Run() override;
protected:
	tMarkupFileSend(const tRunParms& parms,
			const char * filename,
			const char *mimetype,
			const char *httpstatus);
	// presets some default properties for header/footer/etc.
	void SendRaw(const char *pBuf, size_t len);
	const char *m_sFileName, *m_sMimeType, *m_sHttpCode;
	bool m_bFatalError=false;

	// uses fallback lookup map, can be feed with data in subclass constructor
	virtual void SendProp(cmstring &key);
	// XXX: could make this virtual and customizable, if needed
	int CheckCondition(LPCSTR key, size_t len); // 0: true, 1: false, <0: unknown condition

private:
	tMarkupFileSend(const tMarkupFileSend&) =delete;
	tMarkupFileSend operator=(const tMarkupFileSend&)=delete;
	void SendIfElse(LPCSTR pszBeginSep, LPCSTR pszEnd);
};

struct tStyleCss : public tMarkupFileSend
{
	inline tStyleCss(const tRunParms& parms) :
	tMarkupFileSend(parms, "style.css", "text/css", "200 OK") {};
};

class tDeleter : public tMarkupFileSend
{
	tStrDeq files;
	tSS sHidParms;
public:
	tDeleter(const tRunParms& parms);
	virtual void SendProp(cmstring &key) override;
};

struct tShowInfo : public tMarkupFileSend
{
	tShowInfo(const tRunParms& parms)
	:tMarkupFileSend(parms, "userinfo.html", "text/html", "404 Usage Information") {};
};

struct tMaintPage : public tMarkupFileSend
{
	tMaintPage(const tRunParms& parms);
	virtual void SendProp(cmstring &key) override;
};
#endif /*SHOWINFO_H_*/
