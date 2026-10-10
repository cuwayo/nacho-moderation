/**
 * @file tls.c
 * @brief OpenSSL memory-BIO TLS adapter.
 *
 * SAFETY-CRITICAL: the SSL object never owns the socket. Both BIOs are memory
 * BIOs; the io_uring loop moves ciphertext explicitly. This keeps TLS fully
 * non-blocking (SPEC §3, §13).
 *
 * @ingroup tls
 */
#include "tg/tls.h"

#include <errno.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <stdlib.h>
#include <string.h>

/** @brief One TLS session: an SSL object with memory BIOs (no socket). */
struct tls_session {
    SSL_CTX *ctx;    /**< Context (one per session in M1; pooled later). */
    SSL *ssl;        /**< SSL object with memory BIOs. */
    BIO *rbio;       /**< Read BIO (ciphertext in). */
    BIO *wbio;       /**< Write BIO (ciphertext out). */
    int established; /**< 1 once the handshake completed. */
};

/**
 * @brief Map an SSL error to a pump step.
 *
 * @param[in] s   Session.
 * @param[in] ret The SSL_* call's return value.
 * @return TLS_WANT_READ, TLS_WANT_WRITE, or TLS_ERROR.
 */
static tls_step_t step_from_ssl(tls_session_t *s, int ret)
{
    switch (SSL_get_error(s->ssl, ret)) {
    case SSL_ERROR_WANT_READ:
        return TLS_WANT_READ;
    case SSL_ERROR_WANT_WRITE:
        return TLS_WANT_WRITE;
    default:
        /* Includes SSL_ERROR_ZERO_RETURN (clean close) and hard errors; both
         * terminate the operation and are handled by the caller. */
        return TLS_ERROR;
    }
}

tls_session_t *tls_session_new(const tls_config_t *cfg)
{
    if (cfg == NULL || cfg->host == NULL) {
        return NULL;
    }
    tls_session_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }

    s->ctx = SSL_CTX_new(TLS_client_method());
    if (s->ctx == NULL) {
        free(s);
        return NULL;
    }
    if (cfg->verify) {
        SSL_CTX_set_verify(s->ctx, SSL_VERIFY_PEER, NULL);
        SSL_CTX_set_default_verify_paths(s->ctx);
    } else {
        SSL_CTX_set_verify(s->ctx, SSL_VERIFY_NONE, NULL);
    }

    s->ssl = SSL_new(s->ctx);
    s->rbio = BIO_new(BIO_s_mem());
    s->wbio = BIO_new(BIO_s_mem());
    if (s->ssl == NULL || s->rbio == NULL || s->wbio == NULL) {
        /* BIOs are not yet attached to SSL, so free them explicitly. */
        BIO_free(s->rbio);
        BIO_free(s->wbio);
        s->rbio = NULL;
        s->wbio = NULL;
        tls_session_free(s);
        return NULL;
    }
    BIO_set_mem_eof_return(s->rbio, -1);
    BIO_set_mem_eof_return(s->wbio, -1);
    /* SSL_set_bio transfers ownership of both BIOs to the SSL object, which
     * frees them in SSL_free (called from tls_session_free). */
    SSL_set_bio(s->ssl, s->rbio, s->wbio);
    SSL_set_connect_state(s->ssl);

    if (SSL_set_tlsext_host_name(s->ssl, cfg->host) != 1) {
        tls_session_free(s);
        return NULL;
    }
    if (cfg->verify) {
        X509_VERIFY_PARAM *param = SSL_get0_param(s->ssl);
        X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (X509_VERIFY_PARAM_set1_host(param, cfg->host, 0) != 1) {
            tls_session_free(s);
            return NULL;
        }
    }
    return s;
}

tls_step_t tls_handshake_step(tls_session_t *s)
{
    if (s == NULL) {
        return TLS_ERROR;
    }
    int ret = SSL_do_handshake(s->ssl);
    if (ret == 1) {
        s->established = 1;
        return TLS_OK;
    }
    return step_from_ssl(s, ret);
}

int tls_established(const tls_session_t *s)
{
    return s != NULL && s->established;
}

int tls_feed_in(tls_session_t *s, const void *buf, size_t len)
{
    if (s == NULL || (buf == NULL && len != 0)) {
        return -EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    int n = BIO_write(s->rbio, buf, (int)len);
    if (n <= 0) {
        return -EIO;
    }
    return 0;
}

int tls_drain_out(tls_session_t *s, void *buf, size_t cap, size_t *out_len)
{
    if (s == NULL || buf == NULL || out_len == NULL) {
        return -EINVAL;
    }
    *out_len = 0;
    if (cap == 0) {
        return 0;
    }
    int n = BIO_read(s->wbio, buf, (int)cap);
    if (n < 0) {
        /* -1 with an empty BIO means "nothing pending", not an error. */
        return 0;
    }
    *out_len = (size_t)n;
    return 0;
}

tls_step_t tls_write_plain(tls_session_t *s, const void *buf, size_t len, size_t *accepted)
{
    if (s == NULL || accepted == NULL) {
        return TLS_ERROR;
    }
    *accepted = 0;
    if (len == 0) {
        return TLS_OK;
    }
    int ret = SSL_write(s->ssl, buf, (int)len);
    if (ret > 0) {
        *accepted = (size_t)ret;
        return TLS_OK;
    }
    return step_from_ssl(s, ret);
}

tls_step_t tls_read_plain(tls_session_t *s, void *buf, size_t cap, size_t *got)
{
    if (s == NULL || buf == NULL || got == NULL) {
        return TLS_ERROR;
    }
    *got = 0;
    if (cap == 0) {
        return TLS_OK;
    }
    int ret = SSL_read(s->ssl, buf, (int)cap);
    if (ret > 0) {
        *got = (size_t)ret;
        return TLS_OK;
    }
    return step_from_ssl(s, ret);
}

void tls_session_free(tls_session_t *s)
{
    if (s == NULL) {
        return;
    }
    if (s->ssl != NULL) {
        SSL_free(s->ssl); /* frees rbio/wbio, which SSL_set_bio took ownership of */
    }
    if (s->ctx != NULL) {
        SSL_CTX_free(s->ctx);
    }
    free(s);
}
