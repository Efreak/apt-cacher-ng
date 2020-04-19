/*
 * dnsiter.cc
 *
 *  Created on: 29.10.2019
 *      Author: Eduard Bloch
 */

#include "acfg.h"
#include "dnsiter.h"
#include <arpa/inet.h>

//#include <iostream>
#include "debug.h"

namespace acng
{

tDnsIterator::tDnsIterator(int pf_filter, const evutil_addrinfo* par) : m_cur(par)
{
	m_pf = pf_filter;
}

const evutil_addrinfo* tDnsIterator::next()
{
	while(m_cur)
	{
		if(!m_just_created)
			m_cur = m_cur->ai_next;
		m_just_created = false;
		if(!m_cur) return nullptr;
		// this isn't normal since prefiltered before and is not allowed
		if (m_cur->ai_socktype != SOCK_STREAM || m_cur->ai_protocol != IPPROTO_TCP)
			return nullptr;
		if(m_pf == PF_UNSPEC || m_cur->ai_family == m_pf)
			return m_cur;
	}
	return nullptr;
}

const evutil_addrinfo* tAlternatingDnsIterator::next()
{
	LOGSTARTFUNC;
	dbgline;
	if (m_toggle)
	{
		dbgline;
		auto ret = m_iters[!m_idx].next();
		// EOF at alternative, staying here then
		if(!ret)
		{
			dbgline;
			m_toggle = false;
			return next();
		}
		m_idx = !m_idx;
		return ret;
	}
	dbgline;
	return m_iters[m_idx].next();
}

tAlternatingDnsIterator::tAlternatingDnsIterator(const evutil_addrinfo* parObj) :
		m_idx(false), m_toggle(false)
{
	LOGSTARTFUNCx(uintptr_t(parObj), cfg::conprotos[0], cfg::conprotos[1]);
	if(cfg::conprotos[0] == PF_UNSPEC)
	{
		m_idx = 0;
		m_iters[0] = tDnsIterator(PF_UNSPEC, parObj);
	}
	else if(cfg::conprotos[1] == PF_UNSPEC)
	{
		m_idx = 0;
		m_iters[0] = tDnsIterator(cfg::conprotos[0], parObj);
	}
	else
	{
		m_toggle = true;
		m_idx = 1; // will toggle at start
		m_iters[0] = tDnsIterator(cfg::conprotos[0], parObj);
		m_iters[1] = tDnsIterator(cfg::conprotos[1], parObj);
	}
}
}


