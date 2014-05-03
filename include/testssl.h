#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

// crudely stripped down acng code just for testing purposes
int main()
{

SSL * ssl(NULL);
SSL_CTX *m_ctx(0);
BIO *m_bio(0);
	int hret(0);
	if(!m_ctx)
	{
		m_ctx = SSL_CTX_new(SSLv23_client_method());
		if(!m_ctx)
			goto ssl_init_fail;

	}

	ssl = SSL_new(m_ctx);
	if(!ssl)
		goto ssl_init_fail;

 	SSL_set_connect_state(ssl);
 	SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY
 			| SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
 			| SSL_MODE_ENABLE_PARTIAL_WRITE);

 	if((hret=SSL_set_fd(ssl, hret)) != 1)
 		goto ssl_init_fail_retcode;
	if((hret=SSL_connect(ssl)) != 1)
		goto ssl_init_fail_retcode;

 	m_bio = BIO_new(BIO_f_ssl());
 	BIO_set_ssl(m_bio, ssl, BIO_NOCLOSE);

 	BIO_set_nbio(m_bio, 1);

	hret=SSL_get_verify_result(ssl);
	if( hret != X509_V_OK)
		goto ssl_init_fail;

	return true;

	ssl_init_fail_retcode:
	ssl_init_fail:
	return false;
}
