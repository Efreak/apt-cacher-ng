/*
 * confactory.h
 *
 *  Created on: 15.12.2019
 *      Author: Eduard Bloch
 */

#ifndef INCLUDE_CONFACTORY_H_
#define INCLUDE_CONFACTORY_H_

#include "config.h"
#include <memory>
#include <string>

namespace acng
{
class tcpconnect;
using tDlStreamHandle = std::unique_ptr<tcpconnect>;
namespace cfg { struct IHookHandler; }

class ACNG_API IDlConFactory
{
public:

	struct tConRes
	{
		tDlStreamHandle han;
		std::string serr;
		bool wasReused;
	};
	using funcRetCreated = std::function<void(IDlConFactory::tConRes&&)>;

	/// Moves the connection handle to the reserve pool (resets the specified sptr).
	/// Should only be supplied with IDLE connection handles in a sane state.
	virtual void RecycleIdleConnection(tDlStreamHandle handle) =0;
	virtual tConRes CreateConnected(const std::string &sHostname, const std::string &sPort
			,bool ssl,
			cfg::IHookHandler *pStateTracker
			,int timeout
			,bool mustBeFresh
		) =0;
	virtual ~IDlConFactory() {};
};

inline IDlConFactory::tConRes MAKE_CON_RES(cmstring& msg) { return {tDlStreamHandle(), msg, false}; };
inline IDlConFactory::tConRes MAKE_CON_RES_DUMMY() { return MAKE_CON_RES("500 Cancelled"); };
inline IDlConFactory::tConRes MAKE_CON_RES_NOSSL() { return MAKE_CON_RES("500 SSL_NOT_SUPPORTED_BY_PROXY"); }

}


#endif /* INCLUDE_CONFACTORY_H_ */
