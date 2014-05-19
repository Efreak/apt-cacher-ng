#ifndef SHOWINFO_H_
#define SHOWINFO_H_

#include "maintenance.h"

class tStaticFileSend : public tSpecialRequest
{
public:
	tStaticFileSend(int,
			tSpecialRequest::eMaintWorkType type,
			const char * filename,
			const char *mimetype,
			const char *httpcode);
	virtual ~tStaticFileSend();
	void Run(const mstring &cmd) override;
	virtual void ModContents(mstring & contents, cmstring &cmd);
protected:
	const char *m_sFileName, *m_sMimeType, *m_sHttpCode;
	tStaticFileSend();
private:
	tStaticFileSend(const tStaticFileSend&);
	tStaticFileSend operator=(const tStaticFileSend&);
};


class tDeleter : public tStaticFileSend
{
public:
	inline tDeleter(int fd, tSpecialRequest::eMaintWorkType type)
	: tStaticFileSend(fd, type, "delconfirm.html", "text/html", "200 OK") {};
	virtual void ModContents(mstring & contents, cmstring &cmd) override;
};


#endif /*SHOWINFO_H_*/
