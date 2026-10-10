/* test_conn_sm.c - connection state machine over the fake ring (A5.1, A5.2).
 *
 * Drives connect -> HTTP (plain, no TLS) and exercises -EINTR/-EAGAIN resubmit,
 * short writes, peer close, and the linked-timeout path. No network, no ring.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <cmocka.h>

#include "fake_ring.h"
#include "tg/http.h"
#include "tg/loop.h"
#include "tg/net_conn.h"
#include "tg/util.h"

/* --- Fake net ops: a fixed fd, no real socket. ----------------------------- */

static int fake_sock_create(void *ctx, int family, int type, int proto)
{
    (void)ctx;
    (void)family;
    (void)type;
    (void)proto;
    return 42; /* pretend fd */
}

static int fake_sock_close(void *ctx, int fd)
{
    (void)ctx;
    (void)fd;
    return 0;
}

static const net_ops_t fake_net = {
    .sock_create = fake_sock_create,
    .sock_close = fake_sock_close,
    .ctx = NULL,
};

/* --- Fixture --------------------------------------------------------------- */

typedef struct {
    fake_ring_t ring;
    const loop_ops_t *ops;
    conn_t *conn;
    int cb_status;
    int cb_calls;
} fixture_t;

static void conn_callback(void *ctx, int status)
{
    fixture_t *f = ctx;
    f->cb_status = status;
    f->cb_calls++;
}

static int setup(void **state)
{
    fixture_t *f = calloc(1, sizeof(*f));
    assert_non_null(f);
    f->ops = fake_ring_init(&f->ring);
    *state = f;
    return 0;
}

static int teardown(void **state)
{
    fixture_t *f = *state;
    conn_destroy(f->conn);
    free(f);
    return 0;
}

static struct sockaddr_in make_addr(void)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(443);
    return a;
}

/* --- Tests ----------------------------------------------------------------- */

static void test_connect_submits_connect_and_timeout(void **state)
{
    fixture_t *f = *state;
    f->conn = conn_create(f->ops, &fake_net, &util_clock_ops_real, NULL, conn_callback, f);
    assert_non_null(f->conn);

    struct sockaddr_in addr = make_addr();
    assert_int_equal(conn_connect(f->conn, (struct sockaddr *)&addr, sizeof(addr), 5000), 0);
    assert_int_equal(conn_state(f->conn), CONN_CONNECTING);
    assert_int_equal(fake_ring_pending_count(&f->ring), 2); /* connect + link timeout */
    assert_int_equal(fake_ring_last_type(&f->ring), OP_LINK_TIMEOUT);
}

static void test_connect_intr_resubmits(void **state)
{
    (void)state;
    fixture_t g = {0};
    g.ops = fake_ring_init(&g.ring);
    conn_t *c2 = conn_create(g.ops, &fake_net, &util_clock_ops_real, NULL, conn_callback, &g);
    assert_non_null(c2);
    struct sockaddr_in addr = make_addr();
    assert_int_equal(conn_connect(c2, (struct sockaddr *)&addr, sizeof(addr), 0), 0);
    assert_int_equal(fake_ring_pending_count(&g.ring), 2);
    assert_int_equal(fake_ring_submit_all(&g.ring), 2);

    /* -EINTR on the connect must re-prepare it (pending grows by 1). */
    op_t *connect_op = g.ring.inflight[0].op;
    assert_int_equal(fake_ring_complete(&g.ring, connect_op, -EINTR), 0);
    assert_int_equal(fake_ring_pending_count(&g.ring), 1);
    conn_destroy(c2);
}

static void test_full_plain_http_flow(void **state)
{
    fixture_t *f = *state;
    f->conn = conn_create(f->ops, &fake_net, &util_clock_ops_real, NULL, conn_callback, f);
    struct sockaddr_in addr = make_addr();
    assert_int_equal(conn_connect(f->conn, (struct sockaddr *)&addr, sizeof(addr), 0), 0);

    /* Submit connect + timeout; complete the connect successfully. */
    assert_int_equal(fake_ring_submit_all(&f->ring), 2);
    op_t *connect_op = fake_ring_find(&f->ring, OP_CONNECT);
    assert_int_equal(fake_ring_complete(&f->ring, connect_op, 0), 0);
    assert_int_equal(conn_state(f->conn), CONN_HTTP_WRITE); /* ready to send */

    /* Queue the request; it should be sent. */
    const char *req = "GET /getMe HTTP/1.1\r\nHost: x\r\n\r\n";
    assert_int_equal(conn_send_request(f->conn, req, strlen(req)), 0);
    assert_int_equal(fake_ring_last_type(&f->ring), OP_SEND);
    assert_int_equal(conn_state(f->conn), CONN_HTTP_WRITE);

    /* Complete the send, then expect a recv. */
    assert_int_equal(fake_ring_submit_all(&f->ring), 1);
    op_t *send_op = fake_ring_find(&f->ring, OP_SEND);
    assert_int_equal(fake_ring_complete(&f->ring, send_op, (int)strlen(req)), 0);
    assert_int_equal(conn_state(f->conn), CONN_HTTP_READ);
    assert_int_equal(fake_ring_last_type(&f->ring), OP_RECV);

    /* Feed a complete response; the callback fires with status 0. */
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    assert_int_equal(fake_ring_submit_all(&f->ring), 1);
    op_t *recv_op = fake_ring_find(&f->ring, OP_RECV);
    /* The fake recv must deliver bytes; emulate by copying into its buffer. */
    assert_int_equal(fake_ring_fill_and_complete(&f->ring, recv_op, resp, strlen(resp)), 0);
    assert_int_equal(f->cb_calls, 1);
    assert_int_equal(f->cb_status, 0);
    assert_int_equal(conn_state(f->conn), CONN_DONE);
    assert_int_equal(conn_response(f->conn)->body_len, 2);
}

static void test_short_write_resubmits(void **state)
{
    fixture_t *f = *state;
    f->conn = conn_create(f->ops, &fake_net, &util_clock_ops_real, NULL, conn_callback, f);
    struct sockaddr_in addr = make_addr();
    assert_int_equal(conn_connect(f->conn, (struct sockaddr *)&addr, sizeof(addr), 0), 0);
    assert_int_equal(fake_ring_submit_all(&f->ring), 2);
    assert_int_equal(fake_ring_complete(&f->ring, fake_ring_find(&f->ring, OP_CONNECT), 0), 0);

    const char *req = "GET / HTTP/1.1\r\n\r\n";
    assert_int_equal(conn_send_request(f->conn, req, strlen(req)), 0);
    assert_int_equal(fake_ring_submit_all(&f->ring), 1);
    /* First send accepts only part of the request. */
    assert_int_equal(fake_ring_complete(&f->ring, fake_ring_find(&f->ring, OP_SEND), 5), 0);
    assert_int_equal(conn_state(f->conn), CONN_HTTP_WRITE);
    assert_int_equal(fake_ring_last_type(&f->ring), OP_SEND); /* resubmitted */
}

static void test_linked_timeout_fails(void **state)
{
    fixture_t *f = *state;
    f->conn = conn_create(f->ops, &fake_net, &util_clock_ops_real, NULL, conn_callback, f);
    struct sockaddr_in addr = make_addr();
    assert_int_equal(conn_connect(f->conn, (struct sockaddr *)&addr, sizeof(addr), 0), 0);
    assert_int_equal(fake_ring_submit_all(&f->ring), 2);
    op_t *timeout_op = fake_ring_find(&f->ring, OP_LINK_TIMEOUT);
    assert_int_equal(fake_ring_complete(&f->ring, timeout_op, -ETIME), 0);
    assert_int_equal(conn_state(f->conn), CONN_ERROR);
    assert_int_equal(f->cb_status, -ETIME);
}

static void test_peer_close_before_response_fails(void **state)
{
    fixture_t *f = *state;
    f->conn = conn_create(f->ops, &fake_net, &util_clock_ops_real, NULL, conn_callback, f);
    struct sockaddr_in addr = make_addr();
    assert_int_equal(conn_connect(f->conn, (struct sockaddr *)&addr, sizeof(addr), 0), 0);
    assert_int_equal(fake_ring_submit_all(&f->ring), 2);
    assert_int_equal(fake_ring_complete(&f->ring, fake_ring_find(&f->ring, OP_CONNECT), 0), 0);
    const char *req = "GET / HTTP/1.1\r\n\r\n";
    assert_int_equal(conn_send_request(f->conn, req, strlen(req)), 0);
    assert_int_equal(fake_ring_submit_all(&f->ring), 1);
    assert_int_equal(
        fake_ring_complete(&f->ring, fake_ring_find(&f->ring, OP_SEND), (int)strlen(req)), 0);
    assert_int_equal(fake_ring_submit_all(&f->ring), 1);
    /* recv returns 0 (peer closed) with no complete response. */
    assert_int_equal(fake_ring_complete(&f->ring, fake_ring_find(&f->ring, OP_RECV), 0), 0);
    assert_int_equal(conn_state(f->conn), CONN_ERROR);
    assert_int_equal(f->cb_status, -ECONNRESET);
}

/* Run one full request/response cycle on an already-connected plain connection. */
static void serve_request(fixture_t *f, const char *req, const char *resp)
{
    assert_int_equal(conn_send_request(f->conn, req, strlen(req)), 0);
    assert_int_equal(fake_ring_submit_all(&f->ring), 1);
    op_t *send_op = fake_ring_find(&f->ring, OP_SEND);
    assert_int_equal(fake_ring_complete(&f->ring, send_op, (int)strlen(req)), 0);
    assert_int_equal(fake_ring_submit_all(&f->ring), 1);
    op_t *recv_op = fake_ring_find(&f->ring, OP_RECV);
    assert_int_equal(fake_ring_fill_and_complete(&f->ring, recv_op, resp, strlen(resp)), 0);
}

static void test_keep_alive_reuse_second_request(void **state)
{
    fixture_t *f = *state;
    f->conn = conn_create(f->ops, &fake_net, &util_clock_ops_real, NULL, conn_callback, f);
    struct sockaddr_in addr = make_addr();
    assert_int_equal(conn_connect(f->conn, (struct sockaddr *)&addr, sizeof(addr), 0), 0);
    assert_int_equal(fake_ring_submit_all(&f->ring), 2);
    assert_int_equal(fake_ring_complete(&f->ring, fake_ring_find(&f->ring, OP_CONNECT), 0), 0);

    serve_request(f, "GET /a HTTP/1.1\r\nHost: x\r\n\r\n",
                  "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    assert_int_equal(conn_state(f->conn), CONN_DONE);
    assert_int_equal(f->cb_calls, 1);
    assert_true(conn_keep_alive(f->conn));

    /* Reuse the same socket for a second request: no new connect is submitted. */
    assert_int_equal(conn_reuse_begin(f->conn), 0);
    assert_int_equal(conn_state(f->conn), CONN_HTTP_WRITE);
    serve_request(f, "GET /b HTTP/1.1\r\nHost: x\r\n\r\n",
                  "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nbye");
    assert_int_equal(f->cb_calls, 2);
    assert_int_equal(f->cb_status, 0);
    assert_int_equal(conn_response(f->conn)->body_len, 3);
    assert_memory_equal(conn_response(f->conn)->body, "bye", 3);
}

static void test_connection_close_refuses_reuse(void **state)
{
    fixture_t *f = *state;
    f->conn = conn_create(f->ops, &fake_net, &util_clock_ops_real, NULL, conn_callback, f);
    struct sockaddr_in addr = make_addr();
    assert_int_equal(conn_connect(f->conn, (struct sockaddr *)&addr, sizeof(addr), 0), 0);
    assert_int_equal(fake_ring_submit_all(&f->ring), 2);
    assert_int_equal(fake_ring_complete(&f->ring, fake_ring_find(&f->ring, OP_CONNECT), 0), 0);

    serve_request(f, "GET /a HTTP/1.1\r\n\r\n",
                  "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nok");
    assert_false(conn_keep_alive(f->conn));
    assert_int_equal(conn_reuse_begin(f->conn), -EPROTO);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_connect_submits_connect_and_timeout, setup, teardown),
        cmocka_unit_test_setup_teardown(test_connect_intr_resubmits, setup, teardown),
        cmocka_unit_test_setup_teardown(test_full_plain_http_flow, setup, teardown),
        cmocka_unit_test_setup_teardown(test_short_write_resubmits, setup, teardown),
        cmocka_unit_test_setup_teardown(test_linked_timeout_fails, setup, teardown),
        cmocka_unit_test_setup_teardown(test_peer_close_before_response_fails, setup, teardown),
        cmocka_unit_test_setup_teardown(test_keep_alive_reuse_second_request, setup, teardown),
        cmocka_unit_test_setup_teardown(test_connection_close_refuses_reuse, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
