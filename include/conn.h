
#ifndef _CON_H
#define _CON_H

#include "config.h"
#include "lockable.h"

#include <list>
#include <string>

#define RBUFLEN 16384

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
      
      void LogDataCounts(const mstring & file,
    		  const char *xff,
    		  off_t countIn, off_t countOut, bool bFileIsError);
      
   private:
	   con& operator=(const con&);// { /* ASSERT(!"Don't copy con objects"); */ };
	   con(const con&);// { /* ASSERT(!"Don't copy con objects"); */ };

	      //! Terminate the connection descriptors gracefully
	      void ShutDown();



      int m_confd;
      
      MYSTD::list<job*> m_jobs2send;
      
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
      
      struct __tlogstuff
      {
    	  mstring file, client;
    	  off_t sumIn, sumOut;
    	  bool bFileIsError;
    	  void write();
    	  void reset(const mstring &pNewFile, const mstring &pNewClient, bool bIsError);
    	  inline __tlogstuff() : sumIn(0), sumOut(0), bFileIsError(false) {}
      } logstuff;

#ifdef DEBUG
      UINT m_nProcessedJobs;
#endif
};

#endif
