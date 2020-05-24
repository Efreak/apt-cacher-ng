/*
 * sut.h
 *
 *  Created on: 12.01.2020
 *      Author: ed
 */

#ifndef INCLUDE_SUT_H_
#define INCLUDE_SUT_H_

#ifdef UNDER_TEST
#define SUTPROTECTED public
#define SUTPRIVATE public
#else
#define SUTPROTECTED protected
#define SUTPRIVATE private
#endif

#endif /* INCLUDE_SUT_H_ */
