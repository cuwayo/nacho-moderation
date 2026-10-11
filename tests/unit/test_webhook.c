/* test_webhook.c - inbound webhook server over the fake ring (A5.2).
 *
 * Covers secret verification, request acceptance (method/secret), the accept ->
 * recv -> route -> respond path with a fake ring and fake socket ops, and slot
 * release after the response is sent.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <errno.h>
#include <string.h>
#include <sys/socket.h>

#include <cmocka.h>

#include "fake_ring.h"
#include "tg/http.h"
#include "tg/loop.h"
#include "tg/net_conn.h"
#include "tg/webhook.h"

/* --- Fake socket ops ------------------------------------------------------- */

typedef struct {
    int last_bound_fd;
    int closed;
} fake_net_t;

static int fn_create(void *ctx, int family, int type, int proto)
{
    (void)ctx;
    (void)family;
    (void)type;
    (void)proto;
    return 5; /* a fake fd */
}

static int fn_close(void *ctx, int fd)
{
    (void)fd;
    fake_net_t *f = ctx;
    f->closed++;
    return 0;
}

static int fn_bind(void *ctx, int fd, const struct sockaddr *addr, socklen_t len)
{
    (void)addr;
    (void)len;
    fake_net_t *f = ctx;
    f->last_bound_fd = fd;
    return 0;
}

static int fn_listen(void *ctx, int fd, int backlog)
{
    (void)ctx;
    (void)fd;
    (void)backlog;
    return 0;
}

/* --- Update capture -------------------------------------------------------- */

typedef struct {
    int calls;
    char body[256];
    size_t len;
} capture_t;

static void on_update(const char *body, size_t len, void *ctx)
{
    capture_t *c = ctx;
    c->calls++;
    c->len = len;
    if (len < sizeof(c->body)) {
        memcpy(c->body, body, len);
        c->body[len] = '\0';
    }
}

/* --- Pure helpers ---------------------------------------------------------- */

static void test_verify_secret(void **state)
{
    (void)state;
    assert_int_equal(webhook_verify_secret("", ""), 1);       /* no secret: accept */
    assert_int_equal(webhook_verify_secret("abc", "abc"), 1); /* exact match */
    assert_int_equal(webhook_verify_secret("abc", "abd"), 0);
    assert_int_equal(webhook_verify_secret("abc", "ab"), 0); /* length differs */
    assert_int_equal(webhook_verify_secret("abc", ""), 0);
    assert_int_equal(webhook_verify_secret("abc", "abcd"), 0);
}

static void test_request_ok(void **state)
{
    (void)state;
    int status = 0;
    http_req_t req;
    assert_int_equal(http_req_init(&req, 0), 0);

    const char *post = "POST /w HTTP/1.1\r\nX-Telegram-Bot-Api-Secret-Token: s\r\n\r\n";
    size_t consumed = 0;
    assert_int_equal(http_req_feed(&req, post, strlen(post), &consumed), 1);
    assert_int_equal(webhook_request_ok(&req, "s", &status), 1);
    assert_int_equal(status, 200);

    /* Wrong secret -> 403. */
    assert_int_equal(webhook_request_ok(&req, "other", &status), 0);
    assert_int_equal(status, 403);
    http_req_free(&req);

    /* Wrong method -> 405. */
    http_req_t get;
    assert_int_equal(http_req_init(&get, 0), 0);
    const char *g = "GET /w HTTP/1.1\r\n\r\n";
    assert_int_equal(http_req_feed(&get, g, strlen(g), &consumed), 1);
    assert_int_equal(webhook_request_ok(&get, "", &status), 0);
    assert_int_equal(status, 405);
    http_req_free(&get);
}

/* --- Full accept/recv/respond path ----------------------------------------- */

static void test_accept_recv_route_respond(void **state)
{
    (void)state;
    fake_ring_t fr;
    const loop_ops_t *ops = fake_ring_init(&fr);
    fake_net_t net = {0};
    const net_ops_t netops = {.sock_create = fn_create,
                              .sock_close = fn_close,
                              .sock_bind = fn_bind,
                              .sock_listen = fn_listen,
                              .ctx = &net};
    capture_t cap = {0};

    webhook_config_t cfg = {.ops = ops,
                            .net = &netops,
                            .listen_addr = "127.0.0.1:0",
                            .secret = "s",
                            .on_update = on_update,
                            .ctx = &cap};
    webhook_t *w = webhook_create(&cfg);
    assert_non_null(w);
    assert_int_equal(webhook_start(w), 0);
    assert_int_equal(webhook_listen_fd(w), 5);
    assert_int_equal(net.last_bound_fd, 5);

    /* The accept is prepared; submit it and deliver a connection. */
    assert_true(ops->submit(ops->ctx) >= 1);
    op_t *accept = fake_ring_find(&fr, OP_ACCEPT);
    assert_non_null(accept);
    assert_int_equal(fake_ring_complete(&fr, accept, 6), 0); /* accepted fd 6 */

    /* A recv into the connection was prepared; feed a full request. */
    assert_true(ops->submit(ops->ctx) >= 1);
    op_t *recv = fake_ring_find(&fr, OP_RECV);
    assert_non_null(recv);
    const char *req = "POST /w HTTP/1.1\r\nX-Telegram-Bot-Api-Secret-Token: s\r\n"
                      "Content-Length: 12\r\n\r\n{\"update\":1}";
    assert_int_equal(fake_ring_fill_and_complete(&fr, recv, req, strlen(req)), 0);

    /* The update was routed and a response was queued. */
    assert_int_equal(cap.calls, 1);
    assert_string_equal(cap.body, "{\"update\":1}");

    assert_true(ops->submit(ops->ctx) >= 1);
    op_t *send = fake_ring_find(&fr, OP_SEND);
    assert_non_null(send);
    /* The recorded send buffer begins with the status line. */
    assert_int_equal(fake_ring_complete(&fr, send, 4096), 0); /* pretend it all sent */

    webhook_destroy(w);
    assert_true(net.closed >= 1);
}

static void test_wrong_secret_is_not_routed(void **state)
{
    (void)state;
    fake_ring_t fr;
    const loop_ops_t *ops = fake_ring_init(&fr);
    fake_net_t net = {0};
    const net_ops_t netops = {.sock_create = fn_create,
                              .sock_close = fn_close,
                              .sock_bind = fn_bind,
                              .sock_listen = fn_listen,
                              .ctx = &net};
    capture_t cap = {0};

    webhook_config_t cfg = {.ops = ops,
                            .net = &netops,
                            .listen_addr = "127.0.0.1:0",
                            .secret = "expected",
                            .on_update = on_update,
                            .ctx = &cap};
    webhook_t *w = webhook_create(&cfg);
    assert_non_null(w);
    assert_int_equal(webhook_start(w), 0);

    assert_true(ops->submit(ops->ctx) >= 1);
    op_t *accept = fake_ring_find(&fr, OP_ACCEPT);
    assert_int_equal(fake_ring_complete(&fr, accept, 6), 0);

    assert_true(ops->submit(ops->ctx) >= 1);
    op_t *recv = fake_ring_find(&fr, OP_RECV);
    const char *req = "POST /w HTTP/1.1\r\nX-Telegram-Bot-Api-Secret-Token: wrong\r\n"
                      "Content-Length: 2\r\n\r\n{}";
    assert_int_equal(fake_ring_fill_and_complete(&fr, recv, req, strlen(req)), 0);

    /* Rejected: the callback never fires. */
    assert_int_equal(cap.calls, 0);
    webhook_destroy(w);
}

static void test_get_method_is_rejected(void **state)
{
    (void)state;
    fake_ring_t fr;
    const loop_ops_t *ops = fake_ring_init(&fr);
    fake_net_t net = {0};
    const net_ops_t netops = {.sock_create = fn_create,
                              .sock_close = fn_close,
                              .sock_bind = fn_bind,
                              .sock_listen = fn_listen,
                              .ctx = &net};
    capture_t cap = {0};
    webhook_config_t cfg = {.ops = ops,
                            .net = &netops,
                            .listen_addr = "127.0.0.1:0",
                            .secret = "",
                            .on_update = on_update,
                            .ctx = &cap};
    webhook_t *w = webhook_create(&cfg);
    assert_non_null(w);
    assert_int_equal(webhook_start(w), 0);

    assert_true(ops->submit(ops->ctx) >= 1);
    op_t *accept = fake_ring_find(&fr, OP_ACCEPT);
    assert_int_equal(fake_ring_complete(&fr, accept, 6), 0);

    assert_true(ops->submit(ops->ctx) >= 1);
    op_t *recv = fake_ring_find(&fr, OP_RECV);
    const char *req = "GET /w HTTP/1.1\r\n\r\n";
    assert_int_equal(fake_ring_fill_and_complete(&fr, recv, req, strlen(req)), 0);
    assert_int_equal(cap.calls, 0); /* a GET is never routed */
    webhook_destroy(w);
}

static void test_accept_error_and_backpressure(void **state)
{
    (void)state;
    fake_ring_t fr;
    const loop_ops_t *ops = fake_ring_init(&fr);
    fake_net_t net = {0};
    const net_ops_t netops = {.sock_create = fn_create,
                              .sock_close = fn_close,
                              .sock_bind = fn_bind,
                              .sock_listen = fn_listen,
                              .ctx = &net};
    capture_t cap = {0};
    webhook_config_t cfg = {.ops = ops,
                            .net = &netops,
                            .listen_addr = "127.0.0.1:0",
                            .secret = "",
                            .max_conns = 1, /* one slot: the second accept is dropped */
                            .on_update = on_update,
                            .ctx = &cap};
    webhook_t *w = webhook_create(&cfg);
    assert_non_null(w);
    assert_int_equal(webhook_start(w), 0);
    assert_int_equal(webhook_listen_fd(w), 5);

    /* First accept succeeds and consumes the only slot. */
    assert_true(ops->submit(ops->ctx) >= 1);
    op_t *accept = fake_ring_find(&fr, OP_ACCEPT);
    assert_int_equal(fake_ring_complete(&fr, accept, 6), 0);
    assert_int_equal(webhook_active_conns(w), 1);

    /* Second accept has no capacity: the fd is closed immediately, so exactly
     * one close happened and the connection count is unchanged. */
    assert_true(ops->submit(ops->ctx) >= 1);
    op_t *accept2 = fake_ring_find(&fr, OP_ACCEPT);
    assert_int_equal(fake_ring_complete(&fr, accept2, 7), 0);
    assert_int_equal(net.closed, 1);
    assert_int_equal(webhook_active_conns(w), 1);

    /* A transient accept error re-arms without crashing. */
    assert_true(ops->submit(ops->ctx) >= 1);
    op_t *accept3 = fake_ring_find(&fr, OP_ACCEPT);
    assert_int_equal(fake_ring_complete(&fr, accept3, -EAGAIN), 0);

    webhook_stop(w);
    webhook_destroy(w);
}

static void test_peer_close_releases_slot(void **state)
{
    (void)state;
    fake_ring_t fr;
    const loop_ops_t *ops = fake_ring_init(&fr);
    fake_net_t net = {0};
    const net_ops_t netops = {.sock_create = fn_create,
                              .sock_close = fn_close,
                              .sock_bind = fn_bind,
                              .sock_listen = fn_listen,
                              .ctx = &net};
    capture_t cap = {0};
    webhook_config_t cfg = {.ops = ops,
                            .net = &netops,
                            .listen_addr = "127.0.0.1:0",
                            .secret = "",
                            .on_update = on_update,
                            .ctx = &cap};
    webhook_t *w = webhook_create(&cfg);
    assert_non_null(w);
    assert_int_equal(webhook_start(w), 0);

    assert_true(ops->submit(ops->ctx) >= 1);
    op_t *accept = fake_ring_find(&fr, OP_ACCEPT);
    assert_int_equal(fake_ring_complete(&fr, accept, 6), 0);
    assert_int_equal(webhook_active_conns(w), 1);

    /* Peer closes before sending a complete request: the slot is released. */
    assert_true(ops->submit(ops->ctx) >= 1);
    op_t *recv = fake_ring_find(&fr, OP_RECV);
    assert_int_equal(fake_ring_complete(&fr, recv, 0), 0);
    assert_int_equal(webhook_active_conns(w), 0);

    webhook_destroy(w);
}

/* Regression: a connection accepted after webhook_stop() must be closed, not
 * leaked (there is no re-arm to pick it up). */
static void test_accept_after_stop_closes_fd(void **state)
{
    (void)state;
    fake_ring_t fr;
    const loop_ops_t *ops = fake_ring_init(&fr);
    fake_net_t net = {0};
    const net_ops_t netops = {.sock_create = fn_create,
                              .sock_close = fn_close,
                              .sock_bind = fn_bind,
                              .sock_listen = fn_listen,
                              .ctx = &net};
    capture_t cap = {0};
    webhook_config_t cfg = {.ops = ops,
                            .net = &netops,
                            .listen_addr = "127.0.0.1:0",
                            .secret = "",
                            .on_update = on_update,
                            .ctx = &cap};
    webhook_t *w = webhook_create(&cfg);
    assert_non_null(w);
    assert_int_equal(webhook_start(w), 0);

    /* The accept is outstanding; stop the server, then deliver a connection. */
    assert_true(ops->submit(ops->ctx) >= 1);
    op_t *accept = fake_ring_find(&fr, OP_ACCEPT);
    webhook_stop(w);
    assert_int_equal(fake_ring_complete(&fr, accept, 6), 0);

    /* The accepted fd was closed and no slot was taken. */
    assert_int_equal(net.closed, 1);
    assert_int_equal(webhook_active_conns(w), 0);

    webhook_destroy(w);
}

/* Regression: a transient recv error releases the slot; a partial request keeps
 * the slot open for more data. */
static void test_partial_then_error_releases(void **state)
{
    (void)state;
    fake_ring_t fr;
    const loop_ops_t *ops = fake_ring_init(&fr);
    fake_net_t net = {0};
    const net_ops_t netops = {.sock_create = fn_create,
                              .sock_close = fn_close,
                              .sock_bind = fn_bind,
                              .sock_listen = fn_listen,
                              .ctx = &net};
    capture_t cap = {0};
    webhook_config_t cfg = {.ops = ops,
                            .net = &netops,
                            .listen_addr = "127.0.0.1:0",
                            .secret = "",
                            .on_update = on_update,
                            .ctx = &cap};
    webhook_t *w = webhook_create(&cfg);
    assert_non_null(w);
    assert_int_equal(webhook_start(w), 0);

    assert_true(ops->submit(ops->ctx) >= 1);
    op_t *accept = fake_ring_find(&fr, OP_ACCEPT);
    assert_int_equal(fake_ring_complete(&fr, accept, 6), 0);

    /* A partial request keeps the slot and re-arms the recv. */
    assert_true(ops->submit(ops->ctx) >= 1);
    op_t *recv = fake_ring_find(&fr, OP_RECV);
    const char *partial = "POST /w HTTP/1.1\r\nHost: x\r\n";
    assert_int_equal(fake_ring_fill_and_complete(&fr, recv, partial, strlen(partial)), 0);
    assert_int_equal(webhook_active_conns(w), 1);
    assert_int_equal(cap.calls, 0);

    /* A connection reset then releases the slot. */
    assert_true(ops->submit(ops->ctx) >= 1);
    op_t *recv2 = fake_ring_find(&fr, OP_RECV);
    assert_int_equal(fake_ring_complete(&fr, recv2, -ECONNRESET), 0);
    assert_int_equal(webhook_active_conns(w), 0);

    webhook_destroy(w);
}

/* A loop_ops_t wrapper that fails prep_recv after a set number of successful
 * calls, to exercise the "submission failed -> release the slot" path. */
typedef struct {
    const loop_ops_t *base;
    int recv_calls;
    int fail_after; /* fail when recv_calls > fail_after */
} fail_ops_t;

static int fo_prep_recv(void *ctx, op_t *op, int fd, void *buf, size_t len)
{
    fail_ops_t *f = ctx;
    f->recv_calls++;
    if (f->fail_after >= 0 && f->recv_calls > f->fail_after) {
        return -EAGAIN;
    }
    return f->base->prep_recv(f->base->ctx, op, fd, buf, len);
}

static int fo_prep_accept(void *ctx, op_t *op, int fd, struct sockaddr *a, socklen_t *l)
{
    fail_ops_t *f = ctx;
    return f->base->prep_accept(f->base->ctx, op, fd, a, l);
}

static int fo_prep_send(void *ctx, op_t *op, int fd, const void *buf, size_t len)
{
    fail_ops_t *f = ctx;
    return f->base->prep_send(f->base->ctx, op, fd, buf, len);
}

/* Regression: when prep_recv fails (the SQ ring is momentarily full), the
 * connection slot is released rather than left waiting forever. */
static void test_recv_submit_failure_releases_slot(void **state)
{
    (void)state;
    fake_ring_t fr;
    const loop_ops_t *base = fake_ring_init(&fr);
    fail_ops_t fo = {.base = base, .fail_after = 0}; /* first recv submit fails */
    const loop_ops_t fops = {.prep_accept = fo_prep_accept,
                             .prep_recv = fo_prep_recv,
                             .prep_send = fo_prep_send,
                             .prep_connect = base->prep_connect,
                             .prep_timeout = base->prep_timeout,
                             .prep_link_timeout = base->prep_link_timeout,
                             .prep_read_eventfd = base->prep_read_eventfd,
                             .prep_read = base->prep_read,
                             .prep_close = base->prep_close,
                             .submit = base->submit,
                             .wait_cqe = base->wait_cqe,
                             .wait_cqe_timeout = base->wait_cqe_timeout,
                             .ctx = &fo};
    fake_net_t net = {0};
    const net_ops_t netops = {.sock_create = fn_create,
                              .sock_close = fn_close,
                              .sock_bind = fn_bind,
                              .sock_listen = fn_listen,
                              .ctx = &net};
    capture_t cap = {0};
    webhook_config_t cfg = {.ops = &fops,
                            .net = &netops,
                            .listen_addr = "127.0.0.1:0",
                            .secret = "",
                            .on_update = on_update,
                            .ctx = &cap};
    webhook_t *w = webhook_create(&cfg);
    assert_non_null(w);
    assert_int_equal(webhook_start(w), 0);

    /* Accept a connection; the initial recv submission fails, so the slot is
     * released immediately and no connection is left open. */
    assert_true(base->submit(base->ctx) >= 1);
    op_t *accept = fake_ring_find(&fr, OP_ACCEPT);
    assert_int_equal(fake_ring_complete(&fr, accept, 6), 0);
    assert_int_equal(webhook_active_conns(w), 0);
    assert_int_equal(net.closed, 1);

    webhook_destroy(w);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_verify_secret),
        cmocka_unit_test(test_request_ok),
        cmocka_unit_test(test_accept_recv_route_respond),
        cmocka_unit_test(test_wrong_secret_is_not_routed),
        cmocka_unit_test(test_get_method_is_rejected),
        cmocka_unit_test(test_accept_error_and_backpressure),
        cmocka_unit_test(test_peer_close_releases_slot),
        cmocka_unit_test(test_accept_after_stop_closes_fd),
        cmocka_unit_test(test_partial_then_error_releases),
        cmocka_unit_test(test_recv_submit_failure_releases_slot),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
