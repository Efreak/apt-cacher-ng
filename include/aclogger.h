#ifndef _ACLOGGER_H
#define _ACLOGGER_H

#include "config.h"
#include "meta.h"
#include "acbuf.h"

namespace acng
{

#ifdef DEBUG

struct t_logger
{
	t_logger(const char *szFuncName, const void * ptr); // starts the logger, shifts stack depth
	~t_logger();
	tSS & GetFmter();
	void Write(const char *pFile = nullptr, unsigned int nLine = 0);
private:
	tSS m_strm;
	pthread_t m_id;
	unsigned int m_nLevel;
	const char * m_szName;
	uintptr_t callobj;
	// don't copy
	t_logger(const t_logger&);
	t_logger operator=(const t_logger&);
};
#define USRDBG(msg) LOG(msg)
#else
// print some extra things when user wants debug with non-debug build
#define USRDBG(msg) { if(cfg::debug & log::LOG_DEBUG) {log::err( tSS()<<msg); } }
#endif

namespace log
{

enum ETransferType
	: char
	{
		INDATA = 'I', OUTDATA = 'O', ERRORRQ = 'E'
};

enum ELogFlags
	: uint8_t
	{
		LOG_FLUSH = 1, LOG_MORE = 2, LOG_DEBUG = 4
};

// access internal counters
std::pair<off_t, off_t> GetCurrentCountersInOut();
void ResetOldCounters();
std::pair<off_t, off_t> GetOldCountersInOut(bool calcIncomming = true, bool calcOutgoing = true);

bool ACNG_API open();
void ACNG_API close(bool bReopen);
void transfer(uint64_t bytesIn, uint64_t bytesOut, cmstring& sClient, cmstring& sPath,
		bool bAsError);
void ACNG_API err(const char *msg, const char *client = nullptr);
void misc(const mstring & sLine, const char cLogType = 'M');
inline void err(cmstring &msg)
{
	err(msg.c_str());
}
inline void err(const tSS& msg)
{
	err(msg.c_str());
}
void flush();

void GenerateReport(mstring &);

class tRowData
{
public:
	uint64_t byteIn, byteOut;
	unsigned long reqIn, reqOut;
	time_t from, to;
	tRowData() :
			byteIn(0), byteOut(0), reqIn(0), reqOut(0), from(0), to(0)
	{
	}
	;
	/*
	 tRowData(const tRowData &a) :
	 byteIn(a.byteIn), byteOut(a.byteOut),
	 reqIn(a.reqIn), reqOut(a.reqOut),
	 from(a.from), to(a.to)
	 {
	 };
	 */
private:
	// tRowData & operator=(const tRowData &a);
};

mstring GetStatReport();

}

//#define TIMEFORMAT "%a %d/%m"
#define TIMEFORMAT "%Y-%m-%d %H:%M"

}

#endif
