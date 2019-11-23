#ifndef _CON_H
#define _CON_H

#include "meta.h"
#include "fileio.h"

namespace acng
{
class dlcon;

class conn // : public tRunable
{
	struct Impl;
	Impl *_p;
public:
	conn(unique_fd fdId, const char *client);
	virtual ~conn();
	void WorkLoop();

	dlcon* SetupDownloader();
	void LogDataCounts(cmstring & sFile, const char *xff, off_t nNewIn,
			off_t nNewOut, bool bAsError);
private:
	conn& operator=(const conn&); // { /* ASSERT(!"Don't copy con objects"); */ };
	conn(const conn&); // { /* ASSERT(!"Don't copy con objects"); */ };
};

}

#endif
