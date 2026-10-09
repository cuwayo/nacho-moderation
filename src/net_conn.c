/**
 * @file net_conn.c
 * @brief TCP connection state machine with optional TLS, driven by io_uring.
 *
 * Each connection owns a small set of op_t contexts (connect, linked timeout,
 * send, recv). Completions update the state and call conn_pump(), which drives
 * the next step. See net_conn.h for the state list; every transition carries a
 * `STATE:` comment.
 *
 * @ingroup net
 */
#include "tg/net_conn.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "tg/http.h"
#include "tg/loop.h"
#include "tg/tls.h"
#include "tg/util.h"

#define CONN_DEFAULT_TIMEOUT_MS 10000u
#define CONN_READ_CHUNK 16384u

struct conn {
    op_t connect_op; /**< OP_CONNECT context. */
    op_t timeout_op; /**< OP_LINK_TIMEOUT companion. */
    op_t send_op;    /**< OP_SEND context. */
    op_t recv_op;    /**< OP_RECV context. */

    const loop_ops_t *ops;    /**< Loop vtable. */
    const net_ops_t *net;     /**< Socket vtable. */
    const clock_ops_t *clock; /**< Clock vtable. */

    conn_cb cb;         /**< Completion callback. */
    void *cb_ctx;       /**< Callback context. */
    conn_state_t state; /**< Current state. */
    int status;         /**< Result passed to the callback. */

    int fd;                       /**< Socket fd, or -1. */
    tls_session_t *tls;           /**< TLS session, or NULL for plain HTTP. */
    struct sockaddr_storage peer; /**< Saved connect address (for resubmits). */
    socklen_t peer_len;           /**< Length of peer. */

    char *wbuf;  /**< Bytes queued to send (ciphertext or plain request). */
    size_t wlen; /**< Bytes in wbuf. */
    size_t woff; /**< Bytes already sent. */

    char rbuf[CONN_READ_CHUNK]; /**< Receive buffer. */
    http_resp_t resp;           /**< Response parser. */
    int have_request;           /**< 1 once a request has been queued. */
};

/* --- Real socket ops ------------------------------------------------------- */

static int real_sock_create(void *ctx, int family, int type, int proto)
{
    (void)ctx;
    /* SOCK_NONBLOCK/SOCK_CLOEXEC are unsigned flags; combine in unsigned to
     * avoid the signed-bitwise warning, then cast back for socket(). */
    int flags = (int)((unsigned)type | (unsigned)SOCK_NONBLOCK | (unsigned)SOCK_CLOEXEC);
    int fd = socket(family, flags, proto);
    return fd < 0 ? -errno : fd;
}

static int real_sock_close(void *ctx, int fd)
{
    (void)ctx;
    if (fd < 0) {
        return 0;
    }
    return close(fd) == 0 ? 0 : -errno;
}

const net_ops_t net_ops_real = {
    .sock_create = real_sock_create, .sock_close = real_sock_close, .ctx = NULL};

/* --- Forward declarations -------------------------------------------------- */

static void conn_pump(conn_t *c);
static void conn_fail(conn_t *c, int status);
static void on_connect(struct op *op, int res);
static void on_timeout(struct op *op, int res);
static void on_send(struct op *op, int res);
static void on_recv(struct op *op, int res);

/* --- Completion callback plumbing ------------------------------------------ */

static void fire_cb(conn_t *c)
{
    if (c->cb != NULL) {
        c->cb(c->cb_ctx, c->status);
    }
}

static void conn_fail(conn_t *c, int status)
{
    /* STATE: * -> CONN_ERROR on a terminal error. */
    c->state = CONN_ERROR;
    c->status = status;
    fire_cb(c);
}

/* --- Send/recv submission helpers ------------------------------------------ */

/* Submit a send for the remaining wbuf bytes. Returns 0 or negative errno. */
static int submit_send(conn_t *c)
{
    size_t remaining = c->wlen - c->woff;
    if (remaining == 0) {
        return 0;
    }
    return c->ops->prep_send(c->ops->ctx, &c->send_op, c->fd, c->wbuf + c->woff, remaining);
}

static int submit_recv(conn_t *c)
{
    return c->ops->prep_recv(c->ops->ctx, &c->recv_op, c->fd, c->rbuf, sizeof(c->rbuf));
}

/* Append @p n bytes to the write buffer (used for TLS ciphertext). */
static int wbuf_append(conn_t *c, const void *buf, size_t n)
{
    char *nb = realloc(c->wbuf, c->wlen + n);
    if (nb == NULL) {
        return -ENOMEM;
    }
    c->wbuf = nb;
    memcpy(c->wbuf + c->wlen, buf, n);
    c->wlen += n;
    return 0;
}

/* Drain all pending TLS ciphertext into wbuf. */
static int tls_queue_output(conn_t *c)
{
    char tmp[CONN_READ_CHUNK];
    for (;;) {
        size_t got = 0;
        int rc = tls_drain_out(c->tls, tmp, sizeof(tmp), &got);
        if (rc != 0) {
            return rc;
        }
        if (got == 0) {
            return 0;
        }
        rc = wbuf_append(c, tmp, got);
        if (rc != 0) {
            return rc;
        }
    }
}

/* --- TLS handshake pump ---------------------------------------------------- */

/* Drive the handshake as far as the BIOs allow. Returns 0 to keep pumping,
 * 1 when the handshake is complete, negative errno on failure. */
static int tls_handshake_pump(conn_t *c)
{
    for (;;) {
        tls_step_t st = tls_handshake_step(c->tls);
        if (st == TLS_OK) {
            return 1;
        }
        if (st == TLS_ERROR) {
            return -EPROTO;
        }
        /* TLS_WANT_WRITE: queue ciphertext and send it; TLS_WANT_READ: recv. */
        int rc = tls_queue_output(c);
        if (rc != 0) {
            return rc;
        }
        if (c->wlen > c->woff) {
            return submit_send(c);
        }
        /* Nothing to send: need more ciphertext from the peer. */
        return submit_recv(c);
    }
}

/* --- Central pump ---------------------------------------------------------- */

static void conn_pump(conn_t *c)
{
    switch (c->state) {
    case CONN_TLS_HANDSHAKE: {
        int rc = tls_handshake_pump(c);
        if (rc < 0) {
            conn_fail(c, rc);
            return;
        }
        if (rc == 0) {
            return; /* waiting on the loop */
        }
        /* Handshake complete: the connection is ready to write. */
        c->state = CONN_HTTP_WRITE;
        if (c->have_request) {
            rc = submit_send(c);
            if (rc != 0) {
                conn_fail(c, rc);
            }
        }
        return;
    }
    default:
        return;
    }
}

/* --- Op completions -------------------------------------------------------- */

static void on_connect(struct op *op, int res)
{
    conn_t *c = op->owner;
    if (res == -EINTR || res == -EAGAIN) {
        /* STATE: CONN_CONNECTING -> CONN_CONNECTING on -EINTR/-EAGAIN (resubmit). */
        (void)c->ops->prep_connect(c->ops->ctx, &c->connect_op, c->fd,
                                   (const struct sockaddr *)&c->peer, c->peer_len);
        return;
    }
    if (res < 0) {
        /* STATE: CONN_CONNECTING -> CONN_ERROR on connect failure (or -ETIME). */
        conn_fail(c, res);
        return;
    }
    /* STATE: CONN_CONNECTING -> CONN_TLS_HANDSHAKE (or CONN_HTTP_WRITE) on success. */
    if (c->tls != NULL) {
        c->state = CONN_TLS_HANDSHAKE;
        conn_pump(c);
        return;
    }
    /* Plain HTTP: the connection is ready to write. If a request was already
     * queued while connecting, flush it now; otherwise the caller will call
     * conn_send_request() once it observes CONN_HTTP_WRITE. */
    c->state = CONN_HTTP_WRITE;
    if (c->have_request) {
        int rc = submit_send(c);
        if (rc != 0) {
            conn_fail(c, rc);
        }
    }
}

static void on_timeout(struct op *op, int res)
{
    conn_t *c = op->owner;
    /* A linked timeout that fires because its companion completed is cancelled
     * by the kernel with -ECANCELED/-EALREADY; that is not a failure. Only a
     * genuine expiry (-ETIME) fails the connection. */
    if (res == -ECANCELED || res == -EALREADY) {
        return;
    }
    /* STATE: * -> CONN_ERROR on the linked deadline firing. */
    conn_fail(c, res == 0 ? -ETIME : res);
}

static void on_send(struct op *op, int res)
{
    conn_t *c = op->owner;
    if (res == -EINTR || res == -EAGAIN) {
        (void)submit_send(c);
        return;
    }
    if (res < 0) {
        conn_fail(c, res);
        return;
    }
    c->woff += (size_t)res;

    if (c->woff < c->wlen) {
        /* STATE: CONN_HTTP_WRITE -> CONN_HTTP_WRITE on a short write (resubmit). */
        int rc = submit_send(c);
        if (rc != 0) {
            conn_fail(c, rc);
        }
        return;
    }

    /* All queued bytes sent. */
    if (c->state == CONN_TLS_HANDSHAKE) {
        /* Finished sending a handshake flight; continue the handshake. */
        conn_pump(c);
        return;
    }
    if (c->state == CONN_HTTP_WRITE) {
        /* STATE: CONN_HTTP_WRITE -> CONN_HTTP_READ once the request is fully sent. */
        c->state = CONN_HTTP_READ;
        c->wlen = 0;
        c->woff = 0;
        int rc = submit_recv(c);
        if (rc != 0) {
            conn_fail(c, rc);
        }
    }
}

/* Handle a TLS_WANT_WRITE / TLS_ERROR / no-data step result. Returns 0 to
 * continue the pump, 1 when the response is complete, negative errno on error. */
static int tls_step_aftermath(conn_t *c, tls_step_t st)
{
    if (st == TLS_WANT_WRITE) {
        int rc = tls_queue_output(c);
        if (rc != 0) {
            return rc;
        }
        if (c->wlen > c->woff) {
            return submit_send(c);
        }
    }
    /* TLS_ERROR or TLS_WANT_READ with no plaintext: a completed response is a
     * clean success, anything else is a protocol failure. */
    if (st == TLS_ERROR && c->resp.state != HR_DONE) {
        return -EPROTO;
    }
    return c->resp.state == HR_DONE ? 1 : 0;
}

/* Decrypt available plaintext and feed the HTTP parser; returns 1 done. */
static int tls_read_and_feed(conn_t *c)
{
    char plain[CONN_READ_CHUNK];
    for (;;) {
        size_t got = 0;
        tls_step_t st = tls_read_plain(c->tls, plain, sizeof(plain), &got);
        if (st != TLS_OK || got == 0) {
            return tls_step_aftermath(c, st);
        }
        size_t consumed = 0;
        int rc = http_resp_feed(&c->resp, plain, got, &consumed);
        if (rc != 0) {
            return rc; /* 1 = done, negative = error */
        }
    }
}

/* Feed received bytes into the HTTP parser (decrypting first under TLS).
 * Returns 1 when the response is complete, 0 to keep reading, negative errno. */
static int recv_feed_body(conn_t *c, size_t len)
{
    if (c->tls != NULL) {
        return tls_read_and_feed(c);
    }
    size_t consumed = 0;
    return http_resp_feed(&c->resp, c->rbuf, len, &consumed);
}

/* Handle a peer close (res == 0): success only if the response is complete. */
static void recv_peer_closed(conn_t *c)
{
    if (c->resp.state == HR_DONE) {
        c->state = CONN_DONE;
        c->status = 0;
        fire_cb(c);
    } else {
        conn_fail(c, -ECONNRESET);
    }
}

static void on_recv(struct op *op, int res)
{
    conn_t *c = op->owner;
    if (res == -EINTR || res == -EAGAIN) {
        (void)submit_recv(c);
        return;
    }
    if (res < 0) {
        conn_fail(c, res);
        return;
    }
    if (res == 0) {
        recv_peer_closed(c);
        return;
    }

    /* A recv during the handshake feeds ciphertext to the BIO and re-runs the
     * handshake pump; during the body read it decrypts and parses plaintext. */
    if (c->tls != NULL) {
        int rc = tls_feed_in(c->tls, c->rbuf, (size_t)res);
        if (rc != 0) {
            conn_fail(c, rc);
            return;
        }
        if (c->state == CONN_TLS_HANDSHAKE) {
            conn_pump(c);
            return;
        }
    } else if (c->state != CONN_HTTP_READ) {
        conn_fail(c, -EPROTO);
        return;
    }

    int done = recv_feed_body(c, (size_t)res);
    if (done == 1) {
        /* STATE: CONN_HTTP_READ -> CONN_DONE on a complete response. */
        c->state = CONN_DONE;
        c->status = 0;
        fire_cb(c);
        return;
    }
    if (done < 0) {
        conn_fail(c, done);
        return;
    }
    /* STATE: CONN_HTTP_READ -> CONN_HTTP_READ on a partial response (keep reading). */
    int rc = submit_recv(c);
    if (rc != 0) {
        conn_fail(c, rc);
    }
}

/* --- Public API ------------------------------------------------------------ */

conn_t *conn_create(const loop_ops_t *ops, const net_ops_t *net, const clock_ops_t *clock,
                    const tls_config_t *tls, conn_cb cb, void *cb_ctx)
{
    if (ops == NULL || net == NULL || cb == NULL) {
        return NULL;
    }
    conn_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->ops = ops;
    c->net = net;
    c->clock = clock;
    c->cb = cb;
    c->cb_ctx = cb_ctx;
    c->fd = -1;
    c->state = CONN_IDLE;
    if (http_resp_init(&c->resp, 0) != 0) {
        free(c);
        return NULL;
    }
    if (tls != NULL) {
        c->tls = tls_session_new(tls);
        if (c->tls == NULL) {
            http_resp_free(&c->resp);
            free(c);
            return NULL;
        }
    }

    c->connect_op = (op_t){.type = OP_CONNECT, .owner = c, .on_complete = on_connect, .fd = -1};
    c->timeout_op =
        (op_t){.type = OP_LINK_TIMEOUT, .owner = c, .on_complete = on_timeout, .fd = -1};
    c->send_op = (op_t){.type = OP_SEND, .owner = c, .on_complete = on_send, .fd = -1};
    c->recv_op = (op_t){.type = OP_RECV, .owner = c, .on_complete = on_recv, .fd = -1};
    return c;
}

int conn_connect(conn_t *c, const struct sockaddr *addr, socklen_t addr_len, uint64_t timeout_ms)
{
    if (c == NULL || addr == NULL || addr_len == 0) {
        return -EINVAL;
    }
    if ((size_t)addr_len > sizeof(c->peer)) {
        return -EINVAL;
    }
    memcpy(&c->peer, addr, addr_len);
    c->peer_len = addr_len;

    /* STATE: CONN_IDLE -> CONN_CONNECTING on conn_connect(). */
    int fd = c->net->sock_create(c->net->ctx, addr->sa_family, SOCK_STREAM, 0);
    if (fd < 0) {
        return fd;
    }
    c->fd = fd;
    c->connect_op.fd = fd;
    c->send_op.fd = fd;
    c->recv_op.fd = fd;
    c->state = CONN_CONNECTING;

    uint64_t deadline = timeout_ms == 0 ? CONN_DEFAULT_TIMEOUT_MS : timeout_ms;
    c->connect_op.flags |= OPF_TIMEOUT_LINKED; /* chain the linked deadline */
    int rc = c->ops->prep_connect(c->ops->ctx, &c->connect_op, fd, addr, addr_len);
    if (rc != 0) {
        return rc;
    }
    /* LINK_TIMEOUT cancels the connect if it does not complete in time. */
    rc = c->ops->prep_link_timeout(c->ops->ctx, &c->timeout_op, deadline);
    return rc;
}

int conn_send_request(conn_t *c, const char *buf, size_t len)
{
    if (c == NULL || buf == NULL || len == 0) {
        return -EINVAL;
    }
    if (c->state != CONN_HTTP_WRITE && c->state != CONN_TLS_HANDSHAKE &&
        c->state != CONN_CONNECTING) {
        return -EAGAIN;
    }

    if (c->tls != NULL) {
        /* Encrypt into the write BIO, then queue the ciphertext. */
        size_t accepted = 0;
        tls_step_t st = tls_write_plain(c->tls, buf, len, &accepted);
        if (st == TLS_ERROR) {
            return -EPROTO;
        }
        int rc = tls_queue_output(c);
        if (rc != 0) {
            return rc;
        }
    } else {
        int rc = wbuf_append(c, buf, len);
        if (rc != 0) {
            return rc;
        }
    }
    c->have_request = 1;

    if (c->state == CONN_HTTP_WRITE) {
        return submit_send(c);
    }
    return 0; /* will be flushed once the connection/handshake is ready */
}

const http_resp_t *conn_response(const conn_t *c)
{
    return c == NULL ? NULL : &c->resp;
}

conn_state_t conn_state(const conn_t *c)
{
    return c == NULL ? CONN_CLOSED : c->state;
}

int conn_fd(const conn_t *c)
{
    return c == NULL ? -1 : c->fd;
}

void conn_destroy(conn_t *c)
{
    if (c == NULL) {
        return;
    }
    if (c->fd >= 0) {
        (void)c->net->sock_close(c->net->ctx, c->fd);
        c->fd = -1;
    }
    tls_session_free(c->tls);
    http_resp_free(&c->resp);
    free(c->wbuf);
    free(c);
}
