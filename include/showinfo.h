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
	void SetDefaultProps();
	void SendRaw(const char *pBuf, size_t len);
	const char *m_sFileName, *m_sMimeType, *m_sHttpCode;
	bool m_bFatalError=false;

	// uses fallback lookup map, can be feed with data in subclass constructor
	virtual cmstring& GetProp(cmstring &key, cmstring& altValue);
	//inline void AddProp(cmstring &k, cmstring& v) {m_properties.insert(std::make_pair(k,v));};

	std::list<mstring> scratch;
	// keep a copy of some temporary string there and use reference
	inline mstring& KeepCopy(cmstring& tmp) { scratch.push_back(tmp); return scratch.back();}

private:
	tMarkupFileSend(const tMarkupFileSend&);
	tMarkupFileSend operator=(const tMarkupFileSend&);
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
	virtual cmstring& GetProp(cmstring &key, cmstring& altValue) override;
};

struct tShowInfo : public tMarkupFileSend
{
	tShowInfo(const tRunParms& parms)
	:tMarkupFileSend(parms, "userinfo.html", "text/html", "404 Usage Information") {};
};

struct tMaintPage : public tMarkupFileSend
{
	tMaintPage(const tRunParms& parms);
	virtual cmstring& GetProp(cmstring &key, cmstring& altValue) override;
};


#endif /*SHOWINFO_H_*/
