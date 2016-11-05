
#ifndef _CON_H
#define _CON_H

#include "config.h"
#include "lockable.h"

#include <list>
#include <string>

#define RBUFLEN 16384

namespace acng
{

class dlcon;
class job;
class header;

class conn;
typedef SHARED_PTR<conn> tConPtr;

class conn // : public tRunable
{
   public:
      conn(int fdId, const char *client);
      virtual ~conn();
      
      void WorkLoop();
      
   private:
	   conn& operator=(const conn&);// { /* ASSERT(!"Don't copy con objects"); */ };
	   conn(const conn&);// { /* ASSERT(!"Don't copy con objects"); */ };

	      //! Terminate the connection descriptors gracefully
	      void ShutDown();



      int m_confd;
      
      std::list<job*> m_jobs2send;
      
#ifdef KILLABLE
      // to awake select with dummy data
      int wakepipe[2];
#endif
      
      bool m_bStopActivity;
      
      pthread_t m_dlerthr;
      
      // for jobs
      friend class job;
      bool SetupDownloader(const char *xff);
      dlcon * m_pDlClient;
      mstring m_sClientHost;
      header *m_pTmpHead;
      
      // some accounting
      mstring logFile, logClient;
      off_t fileTransferIn = 0, fileTransferOut = 0;
      bool m_bLogAsError = false;
	  void writeAnotherLogRecord(const mstring &pNewFile, const mstring &pNewClient);

      // This method collects the logged data counts for certain file.
	// Since the user might restart the transfer again and again, the counts are accumulated (for each file path)
	void LogDataCounts(cmstring & file, const char *xff, off_t countIn, off_t countOut,
			bool bAsError);

#ifdef DEBUG
      unsigned m_nProcessedJobs;
#endif
};

}

#endif
