/*
 * dnsbase.h
 *
 *  Created on: 10.05.2020
 *      Author: Eduard Bloch
 */

#ifndef INCLUDE_DNSBASE_H_
#define INCLUDE_DNSBASE_H_

#include "evabase.h"

namespace acng
{
class CAddrInfo;

class tDnsBase
{
	evdns_base* m_evdnsbase;

public:
	tDnsBase(event_base* eb);
	~tDnsBase();

	static std::shared_ptr<tDnsBase> Create(event_base *evbase);

	typedef std::function<void(std::shared_ptr<CAddrInfo>)> tDnsResultReporter;

	// async. DNS resolution on IO thread. Reports result through the reporter.
	void Resolve(cmstring & sHostname, cmstring &sPort, tDnsResultReporter) noexcept;
	// like above but blocking
	std::shared_ptr<CAddrInfo> Resolve(cmstring & sHostname, cmstring &sPort);

};
}



#endif /* INCLUDE_DNSBASE_H_ */
