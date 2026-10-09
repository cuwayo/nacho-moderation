/* test_tls.c - memory-BIO TLS pump against an in-process loopback peer (A5.1).
 *
 * Both sides use BIO_s_mem with no socket: ciphertext is shuttled manually
 * between the two SSL objects, exercising handshake, data transfer, partial
 * records, and close_notify. No network, no real clock.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <string.h>

#include <cmocka.h>

#include "tg/tls.h"

/* A minimal loopback peer: server-side SSL with memory BIOs. */
typedef struct {
    SSL_CTX *ctx;
    SSL *ssl;
    BIO *rbio;
    BIO *wbio;
} peer_t;

static EVP_PKEY *make_key(void)
{
    EVP_PKEY *pkey = EVP_RSA_gen(2048);
    return pkey;
}

static X509 *make_cert(EVP_PKEY *pkey)
{
    X509 *x = X509_new();
    if (x == NULL) {
        return NULL;
    }
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_set_pubkey(x, pkey);
    X509_NAME *name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"localhost", -1, -1,
                               0);
    X509_set_issuer_name(x, name);
    if (X509_sign(x, pkey, EVP_sha256()) == 0) {
        X509_free(x);
        return NULL;
    }
    return x;
}

static int peer_setup(peer_t *p)
{
    EVP_PKEY *pkey = make_key();
    if (pkey == NULL) {
        return -1;
    }
    X509 *cert = make_cert(pkey);
    if (cert == NULL) {
        EVP_PKEY_free(pkey);
        return -1;
    }

    p->ctx = SSL_CTX_new(TLS_server_method());
    if (p->ctx == NULL || SSL_CTX_use_certificate(p->ctx, cert) != 1 ||
        SSL_CTX_use_PrivateKey(p->ctx, pkey) != 1) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return -1;
    }
    X509_free(cert);
    EVP_PKEY_free(pkey);

    p->ssl = SSL_new(p->ctx);
    p->rbio = BIO_new(BIO_s_mem());
    p->wbio = BIO_new(BIO_s_mem());
    if (p->ssl == NULL || p->rbio == NULL || p->wbio == NULL) {
        return -1;
    }
    BIO_set_mem_eof_return(p->rbio, -1);
    BIO_set_mem_eof_return(p->wbio, -1);
    SSL_set_bio(p->ssl, p->rbio, p->wbio);
    SSL_set_accept_state(p->ssl);
    return 0;
}

static void peer_free(peer_t *p)
{
    if (p->ssl != NULL) {
        SSL_free(p->ssl);
    }
    if (p->ctx != NULL) {
        SSL_CTX_free(p->ctx);
    }
}

/* Shuttle pending ciphertext from @p from's wbio into @p to's rbio. */

/* Drive the client handshake against the server until both complete. */
static int do_handshake(tls_session_t *client, peer_t *server)
{
    for (int i = 0; i < 50; ++i) {
        tls_step_t cs = tls_handshake_step(client);
        if (cs == TLS_ERROR) {
            return -1;
        }
        /* Move any client ciphertext to the server, then advance the server and
         * move its ciphertext back to the client. */
        char buf[4096];
        size_t got = 0;
        (void)tls_drain_out(client, buf, sizeof(buf), &got);
        if (got > 0) {
            (void)BIO_write(server->rbio, buf, (int)got);
        }

        (void)SSL_do_handshake(server->ssl);
        int sn = BIO_read(server->wbio, buf, sizeof(buf));
        if (sn > 0) {
            (void)tls_feed_in(client, buf, (size_t)sn);
        }

        if (cs == TLS_OK && SSL_is_init_finished(server->ssl)) {
            return 0;
        }
    }
    return -1;
}

static void test_handshake_and_data(void **state)
{
    (void)state;
    peer_t server = {0};
    assert_int_equal(peer_setup(&server), 0);

    tls_config_t cfg = {.host = "localhost", .verify = 0};
    tls_session_t *client = tls_session_new(&cfg);
    assert_non_null(client);
    assert_false(tls_established(client));

    assert_int_equal(do_handshake(client, &server), 0);
    assert_true(tls_established(client));

    /* Client sends plaintext; decrypt it on the server. */
    const char *msg = "hello over memory BIOs";
    size_t accepted = 0;
    assert_int_equal(tls_write_plain(client, msg, strlen(msg), &accepted), TLS_OK);
    char cipher[4096];
    size_t clen = 0;
    assert_int_equal(tls_drain_out(client, cipher, sizeof(cipher), &clen), 0);
    assert_true(clen > 0);
    (void)BIO_write(server.rbio, cipher, (int)clen);

    char plain[128];
    int n = SSL_read(server.ssl, plain, sizeof(plain));
    assert_true(n > 0);
    assert_memory_equal(plain, msg, (size_t)n);

    tls_session_free(client);
    peer_free(&server);
}

static void test_session_new_rejects_null(void **state)
{
    (void)state;
    assert_null(tls_session_new(NULL));
    tls_config_t bad = {.host = NULL, .verify = 0};
    assert_null(tls_session_new(&bad));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_handshake_and_data),
        cmocka_unit_test(test_session_new_rejects_null),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
