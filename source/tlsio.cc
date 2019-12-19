/*
 * tlsio.cc
 *
 *  Created on: 23.12.2019
 *      Author: Eduard Bloch
 */

#include "meta.h"
#include "tlsio.h"
#include "lockable.h"
#include "acfg.h"


#ifdef HAVE_SSL
#include <openssl/evp.h>
#include "openssl/ssl.h"
#include "openssl/err.h"
#include <openssl/crypto.h>

namespace acng
{
namespace atls
{
std::deque<std::mutex> g_ssl_locks;

void thread_lock_cb(int mode, int which, const char *f, int l)
{
	if (which >= int(g_ssl_locks.size()))
		return; // weird
	if (mode & CRYPTO_LOCK)
		g_ssl_locks[which].lock();
	else
		g_ssl_locks[which].unlock();
}

SSL_CTX *g_ssl_ctx = nullptr;
unsigned long g_ssl_error = 0;
bool bInited = false; // not atomic is good enough

//! Global init helper (might be non-reentrant)
void Init()
{
	if(bInited) return;
	bInited=true;

	SSL_load_error_strings();
	ERR_load_BIO_strings();
	ERR_load_crypto_strings();
	ERR_load_SSL_strings();
	OpenSSL_add_all_algorithms();
	SSL_library_init();

	g_ssl_locks.resize(CRYPTO_num_locks());
	CRYPTO_set_id_callback(get_thread_id_cb); CRYPTO_set_locking_callback(thread_lock_cb);
	g_ssl_ctx = SSL_CTX_new(SSLv23_client_method());
	if (!g_ssl_ctx || !SSL_CTX_load_verify_locations(g_ssl_ctx,
			cfg::cafile.empty() ? nullptr : cfg::cafile.c_str(),
			cfg::capath.empty() ? nullptr : cfg::capath.c_str()))
	{
		g_ssl_error = ERR_get_error();
		return;
	}
}

void Deinit()
{
	if(g_ssl_ctx)
		SSL_CTX_free(g_ssl_ctx);
	g_ssl_locks.clear();
}

ACNG_API SSL_CTX* GetContext()
{
	return g_ssl_ctx;
}

ACNG_API std::string GetInitError()
{
	return g_ssl_error ? (std::string("911 SSL error: ") + ERR_reason_error_string(g_ssl_error)) : std::string();
}

}
}
#endif // HAVE_SSL
