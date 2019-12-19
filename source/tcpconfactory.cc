/*
 * tcpconfactory.cc
 *
 *  Created on: 15.12.2019
 *      Author: Eduard Bloch
 */

#include "tcpconnect.h"
#include "confactory.h"
#include "sockio.h"
#include "evabase.h"

#include "debug.h"

#include <future>

using namespace std;

const struct timeval tmout { TIME_SOCKET_EXPIRE_CLOSE, 23 };

namespace acng
{

void ACNG_API cb_clean_cached_con(evutil_socket_t fd, short what, void* arg);

class ACNG_API dl_con_factory : public IDlConFactory
{
public:
	static unordered_multimap<string, tDlStreamHandle> spareConPool;

	/// Moves the connection handle to the reserve pool (resets the specified sptr).
	/// Should only be supplied with IDLE connection handles which is in sane idle state.
	void RecycleIdleConnection(tDlStreamHandle handle) override;

	static cmstring make_key(bool bUseSsl, cmstring &sHost, cmstring &sPort)
	{
		return std::to_string(bUseSsl) + sHost + "," + sPort;
	}
	static cmstring make_key(const tcpconnect *co)
	{
		if (!co)
			return string();
		return make_key(
				bool(co->m_bUseSsl), co->GetHostname(), co->GetPort());
	}

	static void stashConnection(tDlStreamHandle han)
	{
		if(!han) return;
		auto it = spareConPool.end();
		auto fd = han->m_conFd;
		auto ptr = han.get();
		auto key(make_key(ptr));
		it = spareConPool.emplace(key, move(han));
		it->second->m_cacheIt = it;

		// ok, now handover responsibility to libevent scheme

		// XXX: maybe use event_assign, not event_new?
		if(!ptr->m_pCacheCheckEvent)
		{
			ptr->m_pCacheCheckEvent = event_new(evabase::base, fd, EV_READ|EV_TIMEOUT, cb_clean_cached_con, ptr);
			if(!ptr->m_pCacheCheckEvent)
			{
				spareConPool.erase(it);
				return;
			}
		}
		if(0 != event_add(ptr->m_pCacheCheckEvent, &tmout))
		{
			it->second->m_cacheIt = spareConPool.end();
			spareConPool.erase(it);
		}
	}

	void InvokeCreation(cmstring &sHostname, cmstring &sPort,
			bool ssl, cfg::IHookHandler *pStateTracker,
			int timeout, bool makeNew, funcRetCreated&& resultRep)
	{
		ASSERT(evabase::mainThreadId == this_thread::get_id());
		if (!makeNew)
		{
			auto hit = spareConPool.find(make_key(ssl, sHostname, sPort));
			if (hit != spareConPool.end())
			{
				auto ret = move(hit->second);
				spareConPool.erase(hit);
				ret->m_cacheIt = spareConPool.end();
				event_del(ret->m_pCacheCheckEvent);
				resultRep({move(ret), "", true});
				return;
			}
		}
		tcpconnect::DoConnect(make_unique<tcpconnect>(sHostname, sPort, ssl, pStateTracker),
				timeout, resultRep);
	}

	IDlConFactory::tConRes CreateConnected(
			const std::string &sHost, const std::string &sPort
						,bool ssl,
						cfg::IHookHandler *pStateTracker
						,int timeout
						,bool mustBeFresh) override
	{
#ifndef HAVE_SSL
		return no_ssl_result;
#endif
		promise<IDlConFactory::tConRes> reppro;
		auto io_action = [&](bool canceled) -> void
		{
			if(canceled)
				return reppro.set_value(MAKE_CON_RES_DUMMY());
			funcRetCreated onres = [&reppro](IDlConFactory::tConRes&& r){ reppro.set_value(move(r)); };
			InvokeCreation(sHost, sPort, ssl, pStateTracker, timeout, mustBeFresh, move(onres));
		};
		evabase::Post(move(io_action));
		// block until it returns with a result
		auto ret = reppro.get_future().get();
		if(ssl)
		{
			if(ret.serr.empty() && ret.han)
				ret.serr = ret.han->SSLinit(sHost, sPort);
		}
		return ret;

	}
	static ACNG_API void clear_entry(tcpconnect* c)
	{
		spareConPool.erase(c->m_cacheIt);
	}
};

void ACNG_API cb_clean_cached_con(evutil_socket_t fd, short what, void* arg)
{
	// this is only activated if the socket became readable, therefore DISCONNECTED, or on timeout
	if(!arg)
		return;
	// destroys the connection object too
	dl_con_factory::clear_entry((tcpconnect*)arg);
}

unordered_multimap<string, tDlStreamHandle> ACNG_API dl_con_factory::spareConPool;

void dl_con_factory::RecycleIdleConnection(tDlStreamHandle handle)
{
	if(!handle)
		return;

	LOGSTART2("tcpconnect::RecycleIdleConnection", handle->m_sHostName);

	if(handle->m_pStateObserver)
	{
		handle->m_pStateObserver->OnRelease();
		handle->m_pStateObserver = nullptr;
	}

	if(! cfg::persistoutgoing)
	{
		ldbg("not caching outgoing connections, drop " << handle.get());
		handle.reset();
		return;
	}
	// pass the ownership as a raw pointer, to overcome capture semantics limitation
	evabase::PostOrRun(
			[p=handle.release()](bool cncled) -> void
			{
		tDlStreamHandle handle(p);
		if(cncled) return;
		stashConnection(move(handle));
			}
	);
}

dl_con_factory g_tcp_con_factory;
ACNG_API ::acng::IDlConFactory & GetTcpConFactory()
{
	return g_tcp_con_factory;
}











/*
void dl_con_factory::dump_status()
{
	lockguard __g(spareConPoolMx);
	tSS msg;
	msg << "TCP connection cache:\n";
	for (const auto& x : spareConPool)
	{
		if(! x.second.first)
		{
			msg << "[BAD HANDLE] recycle at " << x.second.second << "\n";
			continue;
		}

		msg << x.second.first->m_conFd << ": for "
				<< get<0>(x.first) << ":" << get<1>(x.first)
				<< ", recycled at " << x.second.second
				<< "\n";
	}
#ifdef DEBUG
	msg << "dbg counts, con: " << nConCount.load()
			<< " , discon: " << nDisconCount.load()
			<< " , reuse: " << nReuseCount.load() << "\n";
#endif

	log::err(msg);
}
*/

} // namespace acng
