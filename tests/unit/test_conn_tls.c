/* test_conn_tls.c - connection state machine with TLS over the fake ring (A5.1).
 *
 * Drives CONN_TLS_HANDSHAKE -> CONN_HTTP_WRITE -> CONN_HTTP_READ by shuttling
 * ciphertext between the connection (via the fake ring's send/recv buffers) and
 * an in-process loopback SSL server. No network, no real ring.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <errno.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <cmocka.h>

#include "fake_ring.h"
#include "tg/loop.h"
#include "tg/net_conn.h"
#include "tg/tls.h"
#include "tg/util.h"

/* --- Loopback TLS server (memory BIOs). ------------------------------------ */

typedef struct {
    SSL_CTX *ctx;
    SSL *ssl;
    BIO *rbio;
    BIO *wbio;
    char out[65536];
    size_t out_len;
} server_t;

static X509 *self_signed_cert(EVP_PKEY *pkey)
{
    X509 *x = X509_new();
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

static int server_init(server_t *s)
{
    EVP_PKEY *pkey = EVP_RSA_gen(2048);
    X509 *cert = self_signed_cert(pkey);
    s->ctx = SSL_CTX_new(TLS_server_method());
    if (SSL_CTX_use_certificate(s->ctx, cert) != 1 || SSL_CTX_use_PrivateKey(s->ctx, pkey) != 1) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return -1;
    }
    X509_free(cert);
    EVP_PKEY_free(pkey);
    s->ssl = SSL_new(s->ctx);
    s->rbio = BIO_new(BIO_s_mem());
    s->wbio = BIO_new(BIO_s_mem());
    BIO_set_mem_eof_return(s->rbio, -1);
    BIO_set_mem_eof_return(s->wbio, -1);
    SSL_set_bio(s->ssl, s->rbio, s->wbio);
    SSL_set_accept_state(s->ssl);
    return 0;
}

static void server_free(server_t *s)
{
    if (s->ssl != NULL) {
        SSL_free(s->ssl);
    }
    if (s->ctx != NULL) {
        SSL_CTX_free(s->ctx);
    }
}

/* Pull ciphertext the server wants to send into out[]. */
static void server_flush(server_t *s)
{
    char buf[8192];
    for (;;) {
        int n = BIO_read(s->wbio, buf, sizeof(buf));
        if (n <= 0) {
            return;
        }
        if (s->out_len + (size_t)n <= sizeof(s->out)) {
            memcpy(s->out + s->out_len, buf, (size_t)n);
            s->out_len += (size_t)n;
        }
    }
}

/* --- Fixture --------------------------------------------------------------- */

static int fake_sock_create(void *ctx, int family, int type, int proto)
{
    (void)ctx;
    (void)family;
    (void)type;
    (void)proto;
    return 42;
}
static int fake_sock_close(void *ctx, int fd)
{
    (void)ctx;
    (void)fd;
    return 0;
}
static const net_ops_t fake_net = {
    .sock_create = fake_sock_create, .sock_close = fake_sock_close, .ctx = NULL};

typedef struct {
    fake_ring_t ring;
    const loop_ops_t *ops;
    conn_t *conn;
    server_t server;
    int cb_status;
    int cb_calls;
} fx_t;

static void on_done(void *ctx, int status)
{
    fx_t *f = ctx;
    f->cb_status = status;
    f->cb_calls++;
}

static int setup(void **state)
{
    fx_t *f = calloc(1, sizeof(*f));
    assert_non_null(f);
    f->ops = fake_ring_init(&f->ring);
    assert_int_equal(server_init(&f->server), 0);
    *state = f;
    return 0;
}

static int teardown(void **state)
{
    fx_t *f = *state;
    conn_destroy(f->conn);
    server_free(&f->server);
    free(f);
    return 0;
}

/* Move every pending SEND's ciphertext into the server's read BIO and complete
 * the op. Returns the number of sends completed. */
static int drain_sends(fx_t *f)
{
    int done = 0;
    for (;;) {
        op_t *send_op = fake_ring_find(&f->ring, OP_SEND);
        if (send_op == NULL) {
            break;
        }
        for (size_t k = 0; k < f->ring.inflight_len; ++k) {
            if (f->ring.inflight[k].op == send_op) {
                (void)BIO_write(f->server.rbio, f->ring.inflight[k].buf,
                                (int)f->ring.inflight[k].len);
                assert_int_equal(
                    fake_ring_complete(&f->ring, send_op, (int)f->ring.inflight[k].len), 0);
                break;
            }
        }
        ++done;
    }
    return done;
}

/* Complete a connect and pump the handshake to completion. */
static void establish(fx_t *f)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(443);

    tls_config_t cfg = {.host = "localhost", .verify = 0};
    f->conn = conn_create(f->ops, &fake_net, &util_clock_ops_real, &cfg, on_done, f);
    assert_non_null(f->conn);
    assert_int_equal(conn_connect(f->conn, (struct sockaddr *)&addr, sizeof(addr), 0), 0);

    assert_int_equal(fake_ring_submit_all(&f->ring), 2);
    assert_int_equal(fake_ring_complete(&f->ring, fake_ring_find(&f->ring, OP_CONNECT), 0), 0);

    /* Pump the handshake: shuttle ciphertext both ways until established.
     * server.out accumulates the server's flight; it is delivered in full to
     * whichever recv op the connection has armed. */
    for (int i = 0; i < 50 && conn_state(f->conn) != CONN_HTTP_WRITE; ++i) {
        (void)fake_ring_submit_all(&f->ring);
        (void)drain_sends(f);

        (void)SSL_do_handshake(f->server.ssl);
        server_flush(&f->server); /* appends to server.out */

        op_t *recv_op = fake_ring_find(&f->ring, OP_RECV);
        if (recv_op != NULL && f->server.out_len > 0) {
            assert_true(f->server.out_len <= 65536);
            assert_int_equal(
                fake_ring_fill_and_complete(&f->ring, recv_op, f->server.out, f->server.out_len),
                0);
            f->server.out_len = 0;
        }
    }
}

static void test_tls_handshake_reaches_http_write(void **state)
{
    fx_t *f = *state;
    establish(f);
    assert_int_equal(conn_state(f->conn), CONN_HTTP_WRITE);
}

static void test_tls_roundtrip_response(void **state)
{
    fx_t *f = *state;
    establish(f);
    assert_int_equal(conn_state(f->conn), CONN_HTTP_WRITE);

    const char *req = "GET /getMe HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert_int_equal(conn_send_request(f->conn, req, strlen(req)), 0);

    /* Send the encrypted request to the server. */
    assert_int_equal(fake_ring_submit_all(&f->ring), (int)fake_ring_pending_count(&f->ring));
    assert_true(drain_sends(f) >= 1);

    /* Read the request on the server and answer. */
    char plain[512];
    int n = SSL_read(f->server.ssl, plain, sizeof(plain));
    assert_true(n > 0);
    const char *body = "{\"ok\":true,\"result\":{\"id\":1,\"is_bot\":true,\"first_name\":\"B\"}}";
    char resp[512];
    int rlen = snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n%s",
                        strlen(body), body);
    (void)SSL_write(f->server.ssl, resp, rlen);
    f->server.out_len = 0;
    server_flush(&f->server);

    /* The completed send armed a recv; submit it, then deliver the response. */
    (void)fake_ring_submit_all(&f->ring);
    op_t *recv_op = fake_ring_find(&f->ring, OP_RECV);
    assert_non_null(recv_op);
    assert_int_equal(
        fake_ring_fill_and_complete(&f->ring, recv_op, f->server.out, f->server.out_len), 0);

    assert_int_equal(f->cb_calls, 1);
    assert_int_equal(f->cb_status, 0);
    assert_int_equal(conn_state(f->conn), CONN_DONE);
    assert_int_equal(conn_response(f->conn)->body_len, strlen(body));
}

/* One encrypted request/response cycle on an established connection. */
static void tls_roundtrip(fx_t *f, const char *req, const char *body)
{
    assert_int_equal(conn_send_request(f->conn, req, strlen(req)), 0);
    assert_int_equal(fake_ring_submit_all(&f->ring), (int)fake_ring_pending_count(&f->ring));
    assert_true(drain_sends(f) >= 1);

    char plain[512];
    int n = SSL_read(f->server.ssl, plain, sizeof(plain));
    assert_true(n > 0);
    char resp[512];
    int rlen = snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n%s",
                        strlen(body), body);
    assert_true(rlen > 0 && (size_t)rlen < sizeof(resp));
    (void)SSL_write(f->server.ssl, resp, rlen);
    f->server.out_len = 0;
    server_flush(&f->server);

    (void)fake_ring_submit_all(&f->ring);
    op_t *recv_op = fake_ring_find(&f->ring, OP_RECV);
    assert_non_null(recv_op);
    assert_int_equal(
        fake_ring_fill_and_complete(&f->ring, recv_op, f->server.out, f->server.out_len), 0);
}

static void test_tls_keep_alive_reuse(void **state)
{
    fx_t *f = *state;
    establish(f);
    assert_int_equal(conn_state(f->conn), CONN_HTTP_WRITE);

    tls_roundtrip(f, "GET /a HTTP/1.1\r\nHost: localhost\r\n\r\n",
                  "{\"ok\":true,\"result\":{\"id\":1}}");
    assert_int_equal(f->cb_calls, 1);
    assert_true(conn_keep_alive(f->conn));

    /* Reuse the same TLS session for a second request. */
    assert_int_equal(conn_reuse_begin(f->conn), 0);
    assert_int_equal(conn_state(f->conn), CONN_HTTP_WRITE);
    tls_roundtrip(f, "GET /b HTTP/1.1\r\nHost: localhost\r\n\r\n",
                  "{\"ok\":true,\"result\":{\"id\":2}}");
    assert_int_equal(f->cb_calls, 2);
    assert_int_equal(f->cb_status, 0);
    assert_memory_equal(conn_response(f->conn)->body, "{\"ok\":true,\"result\":{\"id\":2}}", 28);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_tls_handshake_reaches_http_write, setup, teardown),
        cmocka_unit_test_setup_teardown(test_tls_roundtrip_response, setup, teardown),
        cmocka_unit_test_setup_teardown(test_tls_keep_alive_reuse, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
