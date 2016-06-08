
#ifndef _JOB_H
#define _JOB_H

#include "config.h"
#include "header.h"
#include "acbuf.h"
#include <sys/types.h>
#include "fileitem.h"
#include "maintenance.h"

class con;

class job {


   public:

	   typedef enum
	{
		R_DONE = 0, R_AGAIN = 1, R_DISCON = 2, R_NOTFORUS = 3
	} eJobResult;

	   typedef enum {
	   	STATE_SEND_MAIN_HEAD,
	   	STATE_HEADER_SENT,
	   	STATE_SEND_PLAIN_DATA,
	   	STATE_SEND_CHUNK_HEADER,
	   	STATE_SEND_CHUNK_DATA,
	   	STATE_TODISCON,
	   	STATE_ALLDONE,
	   	STATE_SEND_BUFFER,
	   	STATE_ERRORCONT,
	   	STATE_FINISHJOB
	   } eJobState;

      job(header *h, con *pParent);
      ~job();
      //  __attribute__((externally_visible))  
      
      void PrepareDownload(LPCSTR headBuf);

      /*
       * Start or continue returning the file.
       */
      eJobResult SendData(int confd);
  
   private:
      
	  int m_filefd;
	  con *m_pParentCon;
      
      bool m_bChunkMode;
      bool m_bClientWants2Close;
      bool m_bIsHttp11;
      bool m_bNoDownloadStarted;
      
      eJobState m_state, m_backstate;
      
      tSS m_sendbuf;
      mstring m_sFileLoc; // local_relative_path_to_file
      tSpecialRequest::eMaintWorkType m_eMaintWorkType = tSpecialRequest::workNotSpecial;
      mstring m_sOrigUrl; // local SAFE copy of the originating source
      
      header *m_pReqHead; // copy of users requests header

      fileItemMgmt m_pItem;
      off_t m_nSendPos, m_nCurrentRangeLast;
      off_t m_nAllDataCount;
      
      unsigned long m_nChunkRemainingBytes;
      rechecks::eMatchType m_type;
      
      job(const job&);
      job & operator=(const job&);

      const char * BuildAndEnqueHeader(const fileitem::FiStatus &fistate, const off_t &nGooddataSize, header& respHead);
      fileitem::FiStatus _SwitchToPtItem();
      void SetErrorResponse(const char * errorLine, const char *szLocation=nullptr, const char *bodytext=nullptr);
      void PrepareLocalDownload(const mstring &visPath,
    			const mstring &fsBase, const mstring &fsSubpath);

      bool ParseRange();

      off_t m_nReqRangeFrom, m_nReqRangeTo;
};


class tTraceData: public tStrSet, public base_with_mutex {
public:
	static tTraceData& getInstance();
};

#endif
