
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

class con;
typedef SHARED_PTR<con> tConPtr;

class con // : public tRunable
{
   public:
      con(int fdId, const char *client);
      virtual ~con();
      
      void WorkLoop();
      
      // This method collects the logged data counts for certain file.
      // Since the user might restart the transfer again and again, the counts are accumulated (for each file path)
      void LogDataCounts(cmstring & file,
    		  const char *xff,
    		  off_t countIn, off_t countOut);
      
   private:
	   con& operator=(const con&);// { /* ASSERT(!"Don't copy con objects"); */ };
	   con(const con&);// { /* ASSERT(!"Don't copy con objects"); */ };

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
	  void writeAnotherLogRecord(const mstring &pNewFile, const mstring &pNewClient);

#ifdef DEBUG
      unsigned m_nProcessedJobs;
#endif
};

}

#endif
