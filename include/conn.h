
#ifndef _CON_H
#define _CON_H

#include "config.h"
#include "lockable.h"
#include "dlcon.h"

#include <event.h>

#include <list>
#include <string>

#define RBUFLEN 16384

namespace acng
{

class job;
class header;

//class conn;
//typedef SHARED_PTR<conn> tConPtr;

class conn : public tEventFd // , public std::enable_shared_from_this<conn> // : public tRunable
{
   public:
      conn(cmstring& sClientHost);
      virtual ~conn();

      // when deleted, will release and free this event
      struct event *m_event = 0;

      short socketAction(int fd, short what);

   private:
	   conn& operator=(const conn&);// { /* ASSERT(!"Don't copy con objects"); */ };
	   conn(const conn&);// { /* ASSERT(!"Don't copy con objects"); */ };
      
      // descriptors for download progress watching
      // UNSUBSCRIBING MUST BE HANDLED WITH CARE or file descriptor leak would happen
      // (could do that that more safe with a shared_ptr structure and refcounting but
      // feels like overkill for a simple int value)

      /**
       * Setup wake descriptors as needed and subscribe on ptr. User must unsubscribe later!
       */
      bool Subscribe4updates(tFileItemPtr);
      void UnsubscribeFromUpdates(tFileItemPtr);

      acbuf inBuf;
      std::list<job> m_jobs2send;
      
      // for jobs
      friend class job;
      bool SetupDownloader(const char *xff);

      // the slave - must reset the flag there when reseting this pointer
      dlcon * m_pDlClient = 0;
      //dlcon::tWorkState m_lastDlState = {dlcon::tWorkState::allDone, 0};

      mstring m_sClientHost;
      
      // some accounting
      mstring logFile, logClient;
      off_t fileTransferIn = 0, fileTransferOut = 0;
      bool m_bLogAsError = false;
	  void writeAnotherLogRecord(const mstring &pNewFile, const mstring &pNewClient);

      // This method collects the logged data counts for certain file.
	// Since the user might restart the transfer again and again, the counts are accumulated (for each file path)
	void LogDataCounts(cmstring & file, const char *xff, off_t countIn, off_t countOut,
			bool bAsError);

// scratch area for shared used by jobs
	mstring j_sErrorMsg;
	off_t j_nChunkRemainingBytes = 0;
	off_t j_nRetDataCount = 0, j_nConfirmedSizeSoFar = 0; // what dler allows to send so far
	off_t j_nSendPos = 0; // where the reader is
	off_t j_nFileSendLimit = (MAX_VAL(off_t) - 1); // special limit, abort transmission there
	tSS j_sendbuf;
	int j_filefd = -1;
	header j_respHead;

#warning fixme, m_nFileSendLimit is last byte of the byte after?

	inline void ClearSharedMembers()
	{
		j_sErrorMsg.clear();
		j_nChunkRemainingBytes = j_nRetDataCount = j_nConfirmedSizeSoFar = j_nSendPos = 0;
		j_nFileSendLimit = (MAX_VAL(off_t) - 1);
		j_sendbuf.clear();
		j_filefd = -1;
		j_respHead.clear();
	}


#ifdef DEBUG
      unsigned m_nProcessedJobs;
#endif
};

}

#endif
