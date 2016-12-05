#ifndef _JOB_H
#define _JOB_H

#include "config.h"
#include "header.h"
#include "acbuf.h"
#include <sys/types.h> // XXX: why?
#include "fileitem.h"
#include "maintenance.h"

namespace acng
{

class conn;

class job
{
	friend class conn;

	enum : char
		{
			XSTATE_FINISHED = 0, // processing finished
		XSTATE_DISCON,
		XSTATE_EXT_TRIGGERED,		// reenter SendData when something received from downloader
		XSTATE_CAN_SEND,	// can send right away, no trigger needed
//		XSTATE_WAIT_OUT,	// outgoing client socket is busy
	} m_stateExternal = XSTATE_CAN_SEND;

	enum
		: char
		{
			STATE_WAIT_DL_START,
		STATE_SEND_MAIN_HEAD,
		STATE_HEADER_SENT,
		STATE_SEND_PLAIN_DATA,
		STATE_SEND_CHUNK_HEADER,
		STATE_SEND_CHUNK_DATA,
		STATE_FINISHJOB,

// temp. states
		STATE_SEND_BUFFER
	,STATE_CHECK_DL_PROGRESS // 8
	,STATE_FATAL_ERROR
	} m_stateInternal = STATE_WAIT_DL_START, m_stateBackSend = STATE_FATAL_ERROR;

	void PrepareDownload(LPCSTR headBuf);
	// set values to response a fatal error and close connection
	// caller can add extra payload to j_sendBuf if needed.
#warning fixme ,send closecon, http1.1 as needed, and send states...
	tSS& PrepareErrorResponse();

	/*
	 * Start or continue returning the file.
	 */
	void SendData(int confd);

	conn &m_parent;

	// local data bits calculated in the preparation step
	bool m_bChunkMode = false;
	bool m_bClientWants2Close = false;
	bool m_bIsHttp11 = false;
	bool m_bFitemWasSubscribed = false;

	mstring m_sFileLoc; // local_relative_path_to_file
	tSpecialRequest::eMaintWorkType m_eMaintWorkType =
			tSpecialRequest::workNotSpecial;
	header m_reqHead;
	off_t m_nReqRangeFrom = -1, m_nReqRangeTo = -1;
	tFileItemPtr m_pItem;
	rex::eMatchType m_type = rex::FILE_INVALID;

	job(const job&);
	job & operator=(const job&);

	bool FormatHeader(const fileitem::FiStatus &fistate,
			const off_t &nGooddataSize, const header& respHead,
			bool bHasSendableData, int httpstatus);
	/**
	 * Installs the error message string as fatal error, allocating data as needed.
	 */
	void SetFatalErrorResponse(cmstring& errorLine);
	void SetupFileServing(const mstring &visPath, const mstring &fsBase,
			const mstring &fsSubpath);

	bool ParseRange();

	void unsubscribeItem()
	{
		if(!m_bFitemWasSubscribed && !m_pItem) return;
		m_pItem->notifiers.erase(this);
		m_bFitemWasSubscribed = false;
	}
	void subscribeItem();
public:

	/**
	 * Main constructor. Takes the ownership of header's data!
	 */
	job(header& h, conn& pParent);

	~job();

};

}

#endif
