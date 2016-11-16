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
// pointless				R_AGAIN = 1, // short loop, just restart SendData loop to do some work
		XSTATE_DISCON,
		XSTATE_WAIT_DL,		// reenter SendData when something received from downloader
		XSTATE_CAN_SEND		// can send right away, no trigger needed
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
//		STATE_TODISCON,
//		STATE_ALLDONE,
//		STATE_ERRORCONT,
		STATE_FINISHJOB,

// temp. states
		STATE_SEND_BUFFER
	,STATE_CHECK_DL_PROGRESS // 8
	,STATE_FATAL_ERROR
	} m_stateInternal = STATE_WAIT_DL_START, m_stateBackSend = STATE_FATAL_ERROR;

	enum : char
	{
DLSTATE_NOTNEEDED, // shortcut, tell checks to skip downloader observation
DLSTATE_OUR, DLSTATE_OTHER
	} m_eDlType = DLSTATE_OUR;

	job(header *h, conn& pParent);
	~job();
	//  __attribute__((externally_visible))

	void PrepareDownload(LPCSTR headBuf);

	/*
	 * Start or continue returning the file.
	 */
	void SendData(int confd);

	int m_filefd = -1;
	conn &m_parent;

	bool m_bChunkMode = false;
	bool m_bClientWants2Close = false;
	bool m_bIsHttp11 = false;
	bool m_bFitemWasSubscribed = false;

//	eJobState m_state = STATE_WAIT_DL_START, m_backstate = STATE_SEND_MAIN_HEAD; // current state, and state to return after temporary states

	tSS m_sendbuf;
	mstring m_sFileLoc; // local_relative_path_to_file
	tSpecialRequest::eMaintWorkType m_eMaintWorkType =
			tSpecialRequest::workNotSpecial;
	mstring m_sOrigUrl; // local SAFE copy of the originating source

	header *m_pReqHead = 0; // copy of users requests header

	tFileItemPtr m_pItem;

#warning move to scratch area in parent but with reset function
	off_t m_nSendPos = 0, // where the reader is
			m_nConfirmedSizeSoFar = 0, // what dler allows to send so far
			m_nFileSendLimit = (MAX_VAL(off_t) - 1); // special limit, abort transmission there
#warning fixme, m_nFileSendLimit is last byte of the byte after?

	off_t m_nAllDataCount = 0;

	unsigned long m_nChunkRemainingBytes = 0;
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

	off_t m_nReqRangeFrom = -1, m_nReqRangeTo = -1;

	// HTTP status message to report as fatal error ASAP
	mstring* m_psFatalError = nullptr;
};

}

#endif
