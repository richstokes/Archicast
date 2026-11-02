/* TLS for the browser, over mbedTLS from kos-ports. Certificates are not checked:
 * there's no CA store and no trusted clock, so a link is private from a passive
 * listener but not proof of who is at the other end. The browser says so. */
#include <arch/timer.h>
#include <kos/thread.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <psa/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../net.h"
#include "../web.h"

#define TRY_AGAIN 35

struct web_tls {
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config config;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	int fd;
};

static int bio_send(void *ctx, const unsigned char *data, size_t len)
{
	int n = net_sock_send(*(int *)ctx, data, (int)len);
	return n == -TRY_AGAIN || n == -57 ? MBEDTLS_ERR_SSL_WANT_WRITE :
		n < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : n;
}

static int bio_recv(void *ctx, unsigned char *data, size_t len)
{
	int n = net_sock_recv(*(int *)ctx, data, (int)len);
	return n == -TRY_AGAIN || n == -57 ? MBEDTLS_ERR_SSL_WANT_READ :
		n < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : n;
}

void plat_tls_close(web_tls_t *t)
{
	mbedtls_ssl_free(&t->ssl);
	mbedtls_ssl_config_free(&t->config);
	mbedtls_ctr_drbg_free(&t->drbg);
	mbedtls_entropy_free(&t->entropy);
	free(t);
}

web_tls_t *plat_tls_open(int fd, const char *host)
{
	static const char label[] = "Archicast";
	web_tls_t *t = calloc(1, sizeof(*t));
	uint64_t give_up = timer_ms_gettime64() + 30000;
	int rv;
	if (!t)
		return NULL;
	t->fd = fd;
	mbedtls_ssl_init(&t->ssl);
	mbedtls_ssl_config_init(&t->config);
	mbedtls_ctr_drbg_init(&t->drbg);
	mbedtls_entropy_init(&t->entropy);
	psa_crypto_init();
	if (mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy,
	                          (const unsigned char *)label, sizeof(label) - 1) ||
	    mbedtls_ssl_config_defaults(&t->config, MBEDTLS_SSL_IS_CLIENT,
	                                MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT))
		goto fail;
	mbedtls_ssl_conf_authmode(&t->config, MBEDTLS_SSL_VERIFY_NONE);
	mbedtls_ssl_conf_rng(&t->config, mbedtls_ctr_drbg_random, &t->drbg);
	if (mbedtls_ssl_setup(&t->ssl, &t->config) || mbedtls_ssl_set_hostname(&t->ssl, host))
		goto fail;
	mbedtls_ssl_set_bio(&t->ssl, &t->fd, bio_send, bio_recv, NULL);
	while ((rv = mbedtls_ssl_handshake(&t->ssl)) != 0) {
		if ((rv != MBEDTLS_ERR_SSL_WANT_READ && rv != MBEDTLS_ERR_SSL_WANT_WRITE) ||
		    timer_ms_gettime64() > give_up) {
			printf("tls: handshake failed %d\n", rv);
			goto fail;
		}
		thd_sleep(10);
	}
	return t;
fail:
	plat_tls_close(t);
	return NULL;
}

int plat_tls_read(web_tls_t *t, void *data, int len)
{
	int n = mbedtls_ssl_read(&t->ssl, data, (size_t)len);
	if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
	    n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
		return -TRY_AGAIN;
	if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
		return 0;
	return n < 0 ? -1 : n;
}

int plat_tls_write(web_tls_t *t, const void *data, int len)
{
	int n = mbedtls_ssl_write(&t->ssl, data, (size_t)len);
	if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
		return -TRY_AGAIN;
	return n < 0 ? -1 : n;
}

void plat_thread_start(void *(*fn)(void *))
{
	kthread_attr_t attr;
	memset(&attr, 0, sizeof(attr));
	attr.stack_size = 0x10000;
	attr.label = "web";
	thd_create_ex(&attr, fn, NULL);
}

void plat_sleep_ms(int ms)
{
	thd_sleep(ms);
}

uint64_t plat_wall_ms(void)
{
	return timer_ms_gettime64();
}
