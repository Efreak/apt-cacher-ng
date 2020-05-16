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
	virtual ~IActivity() =default;
	/**
	 * Runs the main loop for a program around the event_base loop.
	 * When finished, clean up some resources left behind (fire off specific events
	 * which have actions that would cause blocking otherwise).
	 */
	virtual int MainLoop() =0;

	/**
	 * Tell the executing loop to cancel unfinished activities and stop.
	 */
	virtual void SignalShutdown() =0;

	/**
	 * Push an action into processing queue. In case operation is not possible, runs the action with the cancel flag (bool argument set to true)
	 */
	virtual void Post(tCancelableAction&&) =0;
	/*
	 * Like Post, but if the thread is the same as main thread, run the action in-place
	 */
	virtual void PostOrRun(tCancelableAction&&) =0;

	virtual std::thread::id GetThreadId() =0;
};

std::unique_ptr<IActivity> MakeSelfThreadedActivity();

}

#endif /* INCLUDE_ACTIVITY_H_ */
