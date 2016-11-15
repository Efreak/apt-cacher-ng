
#ifndef _CON_H
#define _CON_H

#include "config.h"
#include "lockable.h"
#include "dlcon.h"

#include <list>
#include <string>

#define RBUFLEN 16384

namespace acng
{

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
      
      bool dlPaused = false;

   private:
	   conn& operator=(const conn&);// { /* ASSERT(!"Don't copy con objects"); */ };
	   conn(const conn&);// { /* ASSERT(!"Don't copy con objects"); */ };

	      //! Terminate the connection descriptors gracefully
	      void ShutDown();

      int m_confd;
      
      // descriptors for download progress watching
      // UNSUBSCRIBING MUST BE HANDLED WITH CARE or file descriptor leak would happen
      // (could do that that more safe with a shared_ptr structure and refcounting but
      // feels like overkill for a simple int value)
#ifdef HAVE_LINUX_EVENTFD
#warning test legacy pipe
#define fdWakeRead m_wakeventfd
#define fdWakeWrite m_wakeventfd
      int m_wakeventfd = -1;
#else
      // XXX: no, this is not nice but what are the alternatives? Add a int& reference? Wastes memory. Or use raw pointers on members? Cannot be safe WRT class member padding.
      int m_wakepipe[2] = {-1, -1};
#define fdWakeRead m_wakepipe[0]
#define fdWakeWrite m_wakepipe[1]
#endif
      /**
       * Setup wake descriptors as needed and subscribe on ptr. User must unsubscribe later!
       */
      void SetupSubscription(tFileItemPtr);

      std::list<job*> m_jobs2send;
      
#ifdef KILLABLE
      // to awake select with dummy data
      int wakepipe[2];
#endif
      // for jobs
      friend class job;
      bool SetupDownloader(const char *xff);
      dlcon * m_pDlClient;
      dlcon::tWorkState m_lastDlState = {dlcon::tWorkState::allDone, 0};

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

	void Shutdown();

// scratch area for shared used by jobs
	string sErrorMsg;


#ifdef DEBUG
      unsigned m_nProcessedJobs;
#endif
};

}

#endif
