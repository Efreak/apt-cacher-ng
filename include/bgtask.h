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

namespace acng
{

class tSpecOpDetachable : public tSpecialRequest
{
public:
	// forward all constructors, no specials here
	// XXX: oh please, g++ 4.7 is not there yet... using tSpecialRequest::tSpecialRequest;
	inline tSpecOpDetachable(const tSpecialRequest::tRunParms& parms)
	: tSpecialRequest(parms)	{ };

	virtual ~tSpecOpDetachable();

	 /*!
	  * This execution implementation makes sure that only one task runs
	  * in background, it sends regular header/footer printing and controls termination.
	  *
	  * Work is to be done the Action() method implemented by the subclasses.
	  */
	virtual void Run() override;

protected:
	bool CheckStopSignal();

	// to be implemented by subclasses
	virtual void Action() =0;

	void SendChunkLocalOnly(const char *data, size_t size) override;

	void DumpLog(time_t id);

	time_t GetTaskId();

private:

	std::ofstream m_reportStream;

	// XXX: this code sucks and needs a full review. It abuses shared_ptr as stupid reference
	// counter. Originally written with some uber-protective considerations in mind like
	// not letting a listener block the work of an operator by any means.

	protected:
	// value is an ID number assigned to the string (key) in the moment of adding it
	struct pathMemEntry { mstring msg; unsigned id;};
	std::map<mstring,pathMemEntry> m_pathMemory;
	// generates a lookup blob as hidden form parameter
	mstring BuildCompressedDelFileCatalog();

	static base_with_condition g_StateCv;
	static bool g_sigTaskAbort;
	// to watch the log file
	int m_logFd = -1;
};

#ifdef DEBUG
class tBgTester : public tSpecOpDetachable
{
public:
	inline tBgTester(const tSpecialRequest::tRunParms& parms)
	: tSpecOpDetachable(parms)
	{
		m_szDecoFile="maint.html";
	}
	void Action() override;
};
#endif // DEBUG

}

#endif /* BGTASK_H_ */
