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

class tSpecOpDetachable : public tSpecialRequest
{
public:
	// forward all constructors, no specials here
	// XXX: oh please, g++ 4.7 is not there yet... using tSpecialRequest::tSpecialRequest;
	inline tSpecOpDetachable(int fd, tSpecialRequest::eMaintWorkType type)
	: tSpecialRequest(fd, type)	{ };

	virtual ~tSpecOpDetachable();

	 /*!
	  * This execution implementation makes sure that only one task runs
	  * in background, it sends regular header/footer printing and controls termination.
	  *
	  * Work is to be done the Action() method implemented by the subclasses.
	  */
	virtual void Run(const mstring &cmd) override;

protected:
	bool CheckStopSignal();

	// to be implemented by subclasses
	virtual void Action(const mstring &) =0;

	void AfterSendChunk(const char *data, size_t size) override;

	void DumpLog(time_t id);

	time_t GetTaskId();

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
	protected:
	tStrSet m_delCboxFilter;
};

struct tRemoteFileInfo;
// helper to make the code more robust
class ifileprocessor
{
public:
	virtual void HandlePkgEntry(const tRemoteFileInfo &entry) = 0;
	virtual ~ifileprocessor() {};
};

extern pthread_mutex_t abortMx;
extern bool bSigTaskAbort;

#ifdef DEBUG
class tBgTester : public tSpecOpDetachable
{
public:
	inline tBgTester(int fd, tSpecialRequest::eMaintWorkType mode)
	: tSpecOpDetachable(fd, mode)
	{
		m_szDecoFile="maint.html";
	}
	void Action(cmstring &) override;
};
#endif // DEBUG

#endif /* BGTASK_H_ */
