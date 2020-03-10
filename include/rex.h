/*
 * rex.h
 *
 *  Created on: 09.03.2020
 *      Author: Eduard Bloch
 */

#ifndef INCLUDE_REX_H_
#define INCLUDE_REX_H_

#include "config.h"
#include <string>

namespace acng
{
namespace rex
{

const char* ReTest(const char* s);

enum NOCACHE_PATTYPE : bool
{
	NOCACHE_REQ,
	NOCACHE_TGT
};

enum eMatchType : int8_t
{
	FILE_INVALID = -1,
	FILE_SOLID = 0, FILE_VOLATILE, FILE_WHITELIST,
	NASTY_PATH, PASSTHROUGH,
	FILE_SPECIAL_SOLID,
	FILE_SPECIAL_VOLATILE,
	ematchtype_max
};
bool Match(const std::string &in, eMatchType type);

eMatchType GetFiletype(const std::string &);
bool MatchUncacheable(const std::string &, NOCACHE_PATTYPE);
bool CompileUncExpressions(NOCACHE_PATTYPE type, const std::string& pat);
/**
 * Compile all expressions from the config, throw on fatal errors.
 */
void CompileExpressions();
}
}

#endif /* INCLUDE_REX_H_ */
