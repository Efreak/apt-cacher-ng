#ifndef SHOWINFO_H_
#define SHOWINFO_H_

#include "maintenance.h"

class tStaticFileSend : public tWUIPage
{
public:
	tStaticFileSend(int, const char * filename,
			const char *mimetype,
			const char *httpcode);
	virtual ~tStaticFileSend();
	void Run(const mstring &cmd);
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
	inline tDeleter(int fd) : tStaticFileSend(fd, "delconfirm.html", "text/html", "200 OK") {}
	virtual void ModContents(mstring & contents, cmstring &cmd);
};


#endif /*SHOWINFO_H_*/
