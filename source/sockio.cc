
#include "meta.h"
#include "sockio.h"
#include "debug.h"

#include <unordered_map>
#include <mutex>

namespace acng
{
using namespace std;

// those data structures are used by main thread only
// helper structure with metadata which can be passed around
unordered_map<int,time_t> g_discoTimeouts;
char crapbuf[40];

void termsocket_now(int fd, void *p = nullptr)
{
	::shutdown(fd, SHUT_RD);
	forceclose(fd);
	g_discoTimeouts.erase(fd);
	if(p) event_free((event*)p);
}

void linger_read(int fd, short what, void *p)
{
	if ((what & EV_TIMEOUT) || !(what & EV_READ))
		return termsocket_now(fd, p);
	// ok, have to read junk or terminating zero-read
	while (true)
	{
		int r = recv(fd, crapbuf, sizeof(crapbuf), MSG_WAITALL);
		if (0 == r)
			return termsocket_now(fd, p);
		if (r < 0)
		{
			if (errno == EAGAIN)
			{
				// come again later
				CTimeVal exp;
				event_add((event*) p, exp.Remaining(g_discoTimeouts[fd]));
				return;
			}
			if (errno == EINTR)
				continue;
			// some other error? Kill it
			return termsocket_now(fd, p);
		}
	}
}

/*! \brief Helper to flush data stream contents reliable and close the connection then
 * DUDES, who write TCP implementations... why can this just not be done easy and reliable? Why do we need hacks like the method below?
 For details, see: http://blog.netherlabs.nl/articles/2009/01/18/the-ultimate-so_linger-page-or-why-is-my-tcp-not-reliable
 * Using SO_LINGER is also dangerous, see https://www.nybek.com/blog/2015/04/29/so_linger-on-non-blocking-sockets
 *
 */
void termsocket_async(int fd, event_base* base)
{
	event* ev(nullptr);
	try
	{
		LOGSTART2s("::termsocket", fd);
		if (!fd)
			return;
		// initiate shutdown, i.e. sending FIN and giving the remote some time to confirm
		::shutdown(fd, SHUT_WR);
		int r = read(fd, crapbuf, sizeof(crapbuf));
		if (r == 0) // fine, we are done
			return termsocket_now(fd, nullptr);
		LOG("waiting for peer to react");
		auto ev = event_new(base, fd, EV_READ, linger_read,
				event_self_cbarg());
		g_discoTimeouts[fd] = GetTime() + cfg::discotimeout;
		struct timeval tmout { cfg::discotimeout, 42 };
		if (ev && 0 == event_add(ev, &tmout))
			return; // will cleanup in the callbacks
	} catch (...)
	{
	}
	// error cleanup... EOM?
	if (ev)
		event_free(ev);
	justforceclose(fd);
}

void set_connect_sock_flags(evutil_socket_t fd)
{
#ifndef NO_TCP_TUNNING
		int yes(1);
		::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
		::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
#endif
		evutil_make_socket_nonblocking(fd);
}

bool ACNG_API isUdsAccessible(cmstring& path)
{
	Cstat s(path);
	return s && S_ISSOCK(s.st_mode) && 0 == access(path.c_str(), W_OK);
}

std::string formatIpPort(const evutil_addrinfo *p)
{
	char buf[300], pbuf[30];
	getnameinfo(p->ai_addr, p->ai_addrlen, buf, sizeof(buf), pbuf, sizeof(pbuf),
			NI_NUMERICHOST | NI_NUMERICSERV);
	return string(p->ai_family == PF_INET6 ? "[" : "") +
			buf +
			(p->ai_family == PF_INET6 ? "]" : "") +
			":" + pbuf;
}


#ifdef HAVE_SSL
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <cstring>
bool DecodeBase64(LPCSTR pAscii, size_t len, acbuf& binData)
{
   if(!pAscii)
      return false;
   binData.setsize(len);
   binData.clear();
   FILE* memStrm = ::fmemopen( (void*) pAscii, len, "r");
   auto strmBase = BIO_new(BIO_f_base64());
   auto strmBin = BIO_new_fp(memStrm, BIO_NOCLOSE);
   strmBin = BIO_push(strmBase, strmBin);
   BIO_set_flags(strmBin, BIO_FLAGS_BASE64_NO_NL);
   binData.got(BIO_read(strmBin, binData.wptr(), len));
   BIO_free_all(strmBin);
   checkForceFclose(memStrm);
   return binData.size();
}
#endif

}
