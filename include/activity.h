/*
 * activity.h
 *
 *  Created on: 09.04.2020
 *      Author: Eduard Bloch
 */

#ifndef INCLUDE_ACTIVITY_H_
#define INCLUDE_ACTIVITY_H_

#include <functional>
#include <thread>
#include <memory>

namespace acng
{
using tCancelableAction = std::function<void(bool)>;

class IActivity
{
public:
	enum JobAcceptance { ACCEPTED = 1, NOT_ACCEPTED = 0, FINISHED = -1 };

	virtual ~IActivity() =default;

	/**
	 * Push an action into processing queue. In case operation is not possible, runs the action with the cancel flag (bool argument set to true)
	 * @return non-zero if job is accepted, false if system is in shutdown mode already
	 */
	virtual JobAcceptance Post(tCancelableAction&&) =0;
	/*
	 * Like Post, but if the thread is the same as main thread, run the action in-place
	 * @return JobAcceptance.ACCEPTED if job is postponed, JobAcceptance.FINISHED if it was run in-place, NOT_ACCEPTED in shutdown condition
	 */
	virtual JobAcceptance PostOrRun(tCancelableAction&&) =0;

	virtual std::thread::id GetThreadId() =0;

	virtual void StartShutdown() =0;
};

std::unique_ptr<IActivity> MakeSelfThreadedActivity();

}

#endif /* INCLUDE_ACTIVITY_H_ */
