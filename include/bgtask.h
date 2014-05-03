/*
 * bgtask.h
 *
 *  Created on: 18.09.2009
 *      Author: ed
 */

#ifndef BGTASK_H_
#define BGTASK_H_

#include "maintenance.h"
#include "lockable.h"
#include <iostream>
#include <fstream>

class tWuiBgTask : public tWUIPage
{
public:
	tWuiBgTask(int);
	virtual ~tWuiBgTask();

	 /*!
	  * This execution implementation makes sure that only one task runs
	  * in background, it sends regular header/footer printing and controls termination.
	  *
	  * Work is to be done the Action() method implemented by the subclasses.
	  */
	virtual void Run(const mstring &cmd);

protected:
	bool CheckStopSignal();

	// to be implemented by subclasses
	virtual void Action(const mstring &) =0;

	void AfterSendChunk(const char *data, size_t size);

	void DumpLog(time_t id);

	time_t GetTaskId();

	bool m_bShowControls;
	mstring m_sTypeName;

private:

	MYSTD::ofstream m_reportStream;

	// XXX: this code sucks and needs a full review. It abuses shared_ptr as stupid reference
	// counter. Originally written with some uber-protective considerations in mind like
	// not letting a listener block the work of an operator by any means.

	// notification of attached processes
	class tProgressTracker : public condition
	{
	public:
		off_t nEndSize;
		time_t id;
		tProgressTracker() : nEndSize(-1), id(0) {};
		void SetEnd(off_t);
	};
	typedef SMARTPTR_SPACE::shared_ptr<tProgressTracker> tProgTrackPtr;
	static SMARTPTR_SPACE::weak_ptr<tProgressTracker> g_pTracker;
	tProgTrackPtr m_pTracker;
};

struct tRemoteFileInfo;
// helper to make the code more robust
class ifileprocessor
{
public:
	virtual void HandlePkgEntry(const tRemoteFileInfo &entry, bool bUncompressForChecksum) = 0;
	virtual ~ifileprocessor() {};
};

extern pthread_mutex_t abortMx;
extern bool bSigTaskAbort;

#ifdef DEBUG
class tBgTester : public tWuiBgTask
{
public:
	inline tBgTester(int fd): tWuiBgTask(fd)
	{
		m_szDecoFile="maint.html";
		m_sTypeName="Tick-Tack-Tick-Tack";
	}
	void Action(cmstring &);
};
#endif // DEBUG

#endif /* BGTASK_H_ */
