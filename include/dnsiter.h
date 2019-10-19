/*
 * dnsiter.h
 *
 *  Created on: 29.10.2019
 *      Author: Eduard Bloch
 *
 *  Help structures to iterate over addrinfo results picking only specific protocols.
 */

#ifndef INCLUDE_DNSITER_H_
#define INCLUDE_DNSITER_H_

#include <event2/util.h>

namespace acng
{

// produce a sequence of entries filtered by family type (ipv4, ipv6, or unspec)
struct tDnsIterator
{
	tDnsIterator(int pf_filter, const evutil_addrinfo* par);
	tDnsIterator() = default;
	const evutil_addrinfo* next();
private:
	const evutil_addrinfo* m_cur = nullptr;
	int m_pf = 0, m_just_created = true;
};

// returns an alternating sequence for different families, starting with the preferred one
struct tAlternatingDnsIterator
{
	tDnsIterator m_iters[2];
	bool m_idx, m_toggle;

	const evutil_addrinfo* next();
	tAlternatingDnsIterator(const evutil_addrinfo*);
};

}



#endif /* INCLUDE_DNSITER_H_ */
