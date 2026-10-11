/**
 * @file webhook.c
 * @brief Inbound webhook server (SPEC section 4.1, webhook mode).
 *
 * See webhook.h for the design. The listening socket is bound once at startup;
 * every accept/recv/send is an io_uring operation dispatched by op type. One
 * accept is outstanding at all times while running; each accepted connection gets
 * a slot and is read to completion, verified against the secret token, routed as
 * one Update, answered, and closed.
 *
 * Connection state machine (per accepted connection):
 * @verbatim
 *   WH_READ --request complete--> (verify) --accepted--> route --> WH_WRITE
 *                                          \--rejected--> 403/405 --> WH_WRITE
 *   WH_WRITE --all bytes sent--> closed (slot freed)
 *   any --error/peer close--> closed (slot freed)
 * @endverbatim
 *
 * @ingroup api
 */
#include "tg/webhook.h"

#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "tables.h"
#include "tg/errors.h"
#include "tg/http.h"
#include "tg/loop.h"
#include "tg/net_conn.h"
#include "tg/util.h"

/** @brief Per-connection read buffer size. */
#define WH_READ_CHUNK 16384u

/** @brief One accepted inbound connection. */
typedef struct {
    int in_use;               /**< 1 while the slot serves a connection. */
    int fd;                   /**< Socket fd, or -1. */
    op_t recv_op;             /**< OP_RECV context. */
    op_t send_op;             /**< OP_SEND context. */
    http_req_t req;           /**< Inbound request parser. */
    char rbuf[WH_READ_CHUNK]; /**< Receive buffer. */
    char *wbuf;               /**< Bytes queued to send. */
    size_t wlen;              /**< Bytes in wbuf. */
    size_t woff;              /**< Bytes already sent. */
    struct webhook *w;        /**< Owning server (for the accept re-arm / slot release). */
} wh_conn_t;

/** @brief The webhook server: listen socket, accept op, and connection slots. */
struct webhook {
    webhook_config_t cfg;         /**< Copied configuration. */
    int listen_fd;                /**< Listening socket, or -1. */
    op_t accept_op;               /**< OP_ACCEPT context. */
    struct sockaddr_storage peer; /**< Peer address buffer for accept. */
    socklen_t peer_len;           /**< In/out length of @ref peer. */
    int accepting;                /**< 1 while an accept is outstanding. */
    int stopped;                  /**< 1 once webhook_stop() was called. */
    wh_conn_t *conns;             /**< Connection slots (length max_conns). */
    unsigned active;              /**< Slots currently in use. */
    char host[256];               /**< Host portion of the listen address. */
};

/* --- Secret verification (pure) -------------------------------------------- */

int webhook_verify_secret(const char *expected, const char *got)
{
    /* A missing expected secret must never authorize a request: an empty or NULL
     * expected value rejects (startup validation refuses to run webhook mode
     * without a secret, so this is a defence in depth, not the only gate). */
    if (expected == NULL || expected[0] == '\0') {
        return 0;
    }
    if (got == NULL) {
        return 0;
    }
    size_t el = strlen(expected);
    size_t gl = strlen(got);
    /* Constant-time compare: fold the length difference into the result so the
     * comparison cost does not leak the secret's length. */
    unsigned diff = (unsigned)(el ^ gl);
    for (size_t i = 0; i < el; ++i) {
        /* Cast to unsigned before the XOR so the operands are not promoted to a
         * signed int (bugprone-signed-bitwise). */
        unsigned a = (unsigned char)expected[i];
        unsigned b = i < gl ? (unsigned char)got[i] : 0u;
        diff |= a ^ b;
    }
    return diff == 0;
}

int webhook_request_ok(const http_req_t *req, const char *expected_secret, int *out_status)
{
    if (req == NULL || out_status == NULL) {
        return 0;
    }
    if (req->method != HTTP_METHOD_POST) {
        *out_status = 405;
        return 0;
    }
    if (!webhook_verify_secret(expected_secret, req->secret)) {
        *out_status = 403;
        return 0;
    }
    *out_status = 200;
    return 1;
}

/* --- Connection helpers ---------------------------------------------------- */

/**
 * @brief Free a connection slot and, if still running, nothing else.
 *
 * @param[in] w Server.
 * @param[in] c Connection slot.
 */
static void conn_release(webhook_t *w, wh_conn_t *c)
{
    if (!c->in_use) {
        return;
    }
    if (c->fd >= 0) {
        (void)w->cfg.net->sock_close(w->cfg.net->ctx, c->fd);
        c->fd = -1;
    }
    http_req_free(&c->req);
    free(c->wbuf);
    c->wbuf = NULL;
    c->wlen = 0;
    c->woff = 0;
    c->in_use = 0;
    if (w->active > 0) {
        w->active--;
    }
}

/**
 * @brief Append @p n bytes to a connection's send buffer.
 *
 * @param[in,out] c   Connection.
 * @param[in]     buf Bytes to append.
 * @param[in]     n   Count.
 * @return 0 on success, -ENOMEM on failure.
 */
static int conn_wbuf_append(wh_conn_t *c, const char *buf, size_t n)
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

/**
 * @brief Build and submit the HTTP response for a connection.
 *
 * @param[in,out] c      Connection.
 * @param[in]     status HTTP status to answer with.
 *
 * @return 0 on success, negative errno on failure.
 */
static int conn_respond(wh_conn_t *c, int status)
{
    const char *body = status == 200 ? "{\"ok\":true}" : "{\"ok\":false}";
    char resp[256];
    size_t resp_len = 0;
    int rc = http_response_build(resp, sizeof(resp), status, body, strlen(body), &resp_len);
    if (rc != 0) {
        return rc;
    }
    rc = conn_wbuf_append(c, resp, resp_len);
    if (rc != 0) {
        return rc;
    }
    /* STATE: WH_WRITE: submit the whole response (send the remainder on a short
     * write, handled in on_send). */
    return c->w->cfg.ops->prep_send(c->w->cfg.ops->ctx, &c->send_op, c->fd, c->wbuf, c->wlen);
}

/* --- Op completions -------------------------------------------------------- */

static void on_recv(struct op *op, int res);
static void on_send(struct op *op, int res);
static void arm_accept(webhook_t *w);

/**
 * @brief OP_ACCEPT completion: take the connection and re-arm the accept.
 *
 * @param[in] op  Accept op (owner is the server).
 * @param[in] res Accepted fd, or negative errno.
 */
static void on_accept(struct op *op, int res)
{
    webhook_t *w = op->owner;
    w->accepting = 0;
    if (w->stopped) {
        /* Shutting down: a connection accepted in the meantime would leak, so
         * close it (there is no re-arm). */
        if (res >= 0) {
            (void)w->cfg.net->sock_close(w->cfg.net->ctx, res);
        }
        return;
    }

    if (res < 0) {
        /* -EINTR/-EAGAIN/-ECANCELED are transient: re-arm. Any other error is
         * logged but the accept loop continues (a bad peer must not kill it). */
        if (res != -EINTR && res != -EAGAIN && res != -ECANCELED) {
            util_log(LOG_WARN, "webhook", "accept failed: %d", res);
        }
        arm_accept(w);
        return;
    }

    int fd = res;
    wh_conn_t *slot = NULL;
    for (unsigned i = 0; i < w->cfg.max_conns; ++i) {
        if (!w->conns[i].in_use) {
            slot = &w->conns[i];
            break;
        }
    }
    if (slot == NULL) {
        /* No capacity: drop the connection immediately (back-pressure). */
        (void)w->cfg.net->sock_close(w->cfg.net->ctx, fd);
    } else {
        slot->in_use = 1;
        slot->fd = fd;
        slot->w = w;
        slot->wlen = 0;
        slot->woff = 0;
        slot->wbuf = NULL;
        (void)http_req_init(&slot->req, w->cfg.max_body);
        slot->recv_op = (op_t){.type = OP_RECV, .owner = slot, .on_complete = on_recv, .fd = fd};
        slot->send_op = (op_t){.type = OP_SEND, .owner = slot, .on_complete = on_send, .fd = fd};
        w->active++;
        /* STATE: connection accepted -> WH_READ: arm the first recv. */
        if (w->cfg.ops->prep_recv(w->cfg.ops->ctx, &slot->recv_op, fd, slot->rbuf,
                                  sizeof(slot->rbuf)) != 0) {
            conn_release(w, slot);
        }
    }

    arm_accept(w); /* one accept is always outstanding while running */
}

/**
 * @brief Arm the accept op if the server is running and no accept is outstanding.
 *
 * @param[in] w Server.
 */
static void arm_accept(webhook_t *w)
{
    if (w->stopped || w->accepting) {
        return;
    }
    w->peer_len = sizeof(w->peer);
    if (w->cfg.ops->prep_accept(w->cfg.ops->ctx, &w->accept_op, w->listen_fd,
                                (struct sockaddr *)&w->peer, &w->peer_len) == 0) {
        w->accepting = 1;
    }
}

/* --- Receive/parse --------------------------------------------------------- */

/**
 * @brief Submit the next receive on a connection, releasing it on failure.
 *
 * A failed submission (the SQ ring is momentarily full) would otherwise leave
 * the slot waiting forever; releasing it is the safe outcome.
 *
 * @param[in] c Connection.
 */
static void conn_arm_recv(wh_conn_t *c)
{
    if (c->w->cfg.ops->prep_recv(c->w->cfg.ops->ctx, &c->recv_op, c->fd, c->rbuf,
                                 sizeof(c->rbuf)) != 0) {
        conn_release(c->w, c);
    }
}

/**
 * @brief Submit the response bytes, releasing the connection on failure.
 *
 * @param[in] c Connection.
 */
static void conn_arm_send(wh_conn_t *c)
{
    if (c->w->cfg.ops->prep_send(c->w->cfg.ops->ctx, &c->send_op, c->fd, c->wbuf + c->woff,
                                 c->wlen - c->woff) != 0) {
        conn_release(c->w, c);
    }
}

/**
 * @brief OP_RECV completion: parse the request, route it, and answer.
 *
 * @param[in] op  Recv op (owner is the connection slot).
 * @param[in] res Bytes received, 0 on peer close, or negative errno.
 */
static void on_recv(struct op *op, int res)
{
    wh_conn_t *c = op->owner;
    if (!c->in_use) {
        return; /* a late completion for a slot already released */
    }
    webhook_t *w = c->w;
    if (res == -EINTR || res == -EAGAIN) {
        conn_arm_recv(c);
        return;
    }
    if (res < 0) {
        conn_release(w, c);
        return;
    }
    if (res == 0) {
        conn_release(w, c); /* peer closed before sending a full request */
        return;
    }

    size_t consumed = 0;
    int rc = http_req_feed(&c->req, c->rbuf, (size_t)res, &consumed);
    if (rc == 0) {
        /* STATE: WH_READ -> WH_READ on a partial request (keep reading). */
        conn_arm_recv(c);
        return;
    }

    int status = 400;
    if (rc == 1) {
        int ok = webhook_request_ok(&c->req, w->cfg.secret, &status);
        if (ok) {
            if (w->cfg.on_update != NULL) {
                w->cfg.on_update(c->req.body, c->req.body_len, w->cfg.ctx);
            }
        }
    }

    /* STATE: WH_READ -> WH_WRITE: queue the response and send it. */
    rc = conn_respond(c, status);
    if (rc != 0) {
        conn_release(w, c);
    }
}

/**
 * @brief OP_SEND completion: advance, resubmit, or close the connection.
 *
 * @param[in] op  Send op (owner is the connection slot).
 * @param[in] res Bytes written, or negative errno.
 */
static void on_send(struct op *op, int res)
{
    wh_conn_t *c = op->owner;
    if (!c->in_use) {
        return; /* a late completion for a slot already released */
    }
    webhook_t *w = c->w;
    if (res == -EINTR || res == -EAGAIN) {
        conn_arm_send(c);
        return;
    }
    if (res < 0) {
        conn_release(w, c);
        return;
    }
    c->woff += (size_t)res;
    if (c->woff < c->wlen) {
        /* STATE: WH_WRITE -> WH_WRITE on a short write (resubmit the remainder). */
        conn_arm_send(c);
        return;
    }
    /* STATE: WH_WRITE -> closed on a fully sent response (Connection: close). */
    conn_release(w, c);
}

/* --- Listen setup ---------------------------------------------------------- */

/**
 * @brief Split a "host:port" listen address and resolve it (startup only).
 *
 * @param[in]  addr   Listen address.
 * @param[out] host   Receives the host part.
 * @param[in]  host_cap Capacity of @p host.
 * @param[out] port   Receives the port part.
 * @param[in]  port_cap Capacity of @p port.
 *
 * @return 0 on success, -EINVAL on a malformed address.
 */
static int split_listen_addr(const char *addr, char *host, size_t host_cap, char *port,
                             size_t port_cap)
{
    const char *colon = strrchr(addr, ':');
    if (colon == NULL) {
        return -EINVAL;
    }
    size_t hl = (size_t)(colon - addr);
    if (hl >= host_cap || strlen(colon + 1) >= port_cap) {
        return -EINVAL;
    }
    if (hl == 0) {
        (void)snprintf(host, host_cap, "0.0.0.0"); /* ":8080" binds every interface */
    } else {
        memcpy(host, addr, hl);
        host[hl] = '\0';
    }
    (void)snprintf(port, port_cap, "%s", colon + 1);
    return 0;
}

webhook_t *webhook_create(const webhook_config_t *cfg)
{
    if (cfg == NULL || cfg->ops == NULL || cfg->ops->prep_accept == NULL ||
        cfg->ops->prep_recv == NULL || cfg->ops->prep_send == NULL || cfg->net == NULL ||
        cfg->net->sock_create == NULL || cfg->net->sock_close == NULL ||
        cfg->net->sock_bind == NULL || cfg->net->sock_listen == NULL || cfg->listen_addr == NULL ||
        cfg->on_update == NULL) {
        return NULL;
    }
    webhook_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return NULL;
    }
    w->cfg = *cfg;
    if (w->cfg.max_body == 0) {
        w->cfg.max_body = WEBHOOK_MAX_BODY;
    }
    if (w->cfg.max_conns == 0) {
        w->cfg.max_conns = WEBHOOK_MAX_CONNS;
    }
    w->listen_fd = -1;
    w->conns = calloc(w->cfg.max_conns, sizeof(*w->conns));
    if (w->conns == NULL) {
        free(w);
        return NULL;
    }
    for (unsigned i = 0; i < w->cfg.max_conns; ++i) {
        w->conns[i].fd = -1;
    }
    w->accept_op = (op_t){.type = OP_ACCEPT, .owner = w, .on_complete = on_accept, .fd = -1};
    return w;
}

int webhook_start(webhook_t *w)
{
    if (w == NULL) {
        return -EINVAL;
    }
    char port[8];
    if (split_listen_addr(w->cfg.listen_addr, w->host, sizeof(w->host), port, sizeof(port)) != 0) {
        return -EINVAL;
    }

    /* Resolve once at startup on the loop thread, before the loop runs (the
     * non-blocking rule permits startup I/O; see resolver.h). */
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(w->host, port, &hints, &res) != 0 || res == NULL) {
        return -ERR_NET;
    }

    int fd = w->cfg.net->sock_create(w->cfg.net->ctx, res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        return fd;
    }
    /* Allow a quick restart without "address already in use". */
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    int rc = w->cfg.net->sock_bind(w->cfg.net->ctx, fd, res->ai_addr, (socklen_t)res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) {
        (void)w->cfg.net->sock_close(w->cfg.net->ctx, fd);
        return rc;
    }
    rc = w->cfg.net->sock_listen(w->cfg.net->ctx, fd, 128);
    if (rc != 0) {
        (void)w->cfg.net->sock_close(w->cfg.net->ctx, fd);
        return rc;
    }

    w->listen_fd = fd;
    w->accept_op.fd = fd;
    arm_accept(w);
    return w->accepting ? 0 : -EIO;
}

int webhook_listen_fd(const webhook_t *w)
{
    return w == NULL ? -1 : w->listen_fd;
}

unsigned webhook_active_conns(const webhook_t *w)
{
    return w == NULL ? 0 : w->active;
}

void webhook_stop(webhook_t *w)
{
    if (w != NULL) {
        w->stopped = 1;
    }
}

void webhook_destroy(webhook_t *w)
{
    if (w == NULL) {
        return;
    }
    for (unsigned i = 0; i < w->cfg.max_conns; ++i) {
        conn_release(w, &w->conns[i]);
    }
    if (w->listen_fd >= 0) {
        (void)w->cfg.net->sock_close(w->cfg.net->ctx, w->listen_fd);
        w->listen_fd = -1;
    }
    free(w->conns);
    free(w);
}
