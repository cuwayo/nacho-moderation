/**
 * @file net_conn.h
 * @brief TCP connection state machine driven by the io_uring loop.
 *
 * A connection progresses through connect -> TLS handshake -> HTTP write -> HTTP
 * read. Name resolution is handled separately by @ref resolver.h; the caller
 * passes an already-resolved address to conn_connect(). Every transition is
 * documented in the implementation with a `STATE:` comment (A7). Socket lifecycle
 * goes through the injectable @ref net_ops_t so tests never touch the network.
 *
 * @ingroup net
 */
#ifndef TG_NET_CONN_H
#define TG_NET_CONN_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "tg/http.h"
#include "tg/loop.h"
#include "tg/tls.h"
#include "tg/util.h"

/** @brief Injectable socket operations (SPEC-ADDENDUM-TOOLCHAIN A3). */
typedef struct {
    /** Create a non-blocking socket; returns the fd or negative errno. */
    int (*sock_create)(void *ctx, int family, int type, int proto);
    /** Close a socket (no-op for a negative fd). */
    int (*sock_close)(void *ctx, int fd);
    void *ctx; /**< Implementation state. */
} net_ops_t;

/** @brief Connection lifecycle states. */
typedef enum {
    CONN_IDLE = 0,      /**< Created but not started. */
    CONN_CONNECTING,    /**< TCP connect in flight. */
    CONN_TLS_HANDSHAKE, /**< TLS handshake in progress. */
    CONN_HTTP_WRITE,    /**< Sending the request. */
    CONN_HTTP_READ,     /**< Reading the response. */
    CONN_DONE,          /**< Response complete (success). */
    CONN_ERROR,         /**< Terminal error. */
    CONN_CLOSED         /**< Descriptor closed and resources released. */
} conn_state_t;

/** @brief Connection completion callback: @p status is 0 or negative errno. */
typedef void (*conn_cb)(void *ctx, int status);

/** @brief Opaque connection object (one TCP/TLS connection). */
typedef struct conn conn_t;

/** @brief Real socket ops backed by the syscalls (production use). */
extern const net_ops_t net_ops_real;

/**
 * @brief Create a connection object.
 *
 * @param[in] ops     Loop operations that drive the connection (must outlive it).
 * @param[in] net     Injectable socket ops.
 * @param[in] clock   Injectable clock (deadlines).
 * @param[in] tls     TLS settings (host for SNI/verification; NULL disables TLS).
 * @param[in] cb      Completion callback (never NULL).
 * @param[in] cb_ctx  Opaque pointer passed to @p cb.
 *
 * @return A new connection, or NULL on failure.
 * @ownership Caller must call conn_destroy().
 * @thread_safety Loop thread only.
 */
conn_t *conn_create(const loop_ops_t *ops, const net_ops_t *net, const clock_ops_t *clock,
                    const tls_config_t *tls, conn_cb cb, void *cb_ctx);

/**
 * @brief Begin a non-blocking connect to @p addr, with a linked deadline.
 *
 * Transitions CONN_IDLE -> CONN_CONNECTING. On success the connection proceeds
 * automatically through TLS (if configured) and then to CONN_HTTP_WRITE, at
 * which point the caller should call conn_send_request().
 *
 * @param[in] c        Connection.
 * @param[in] addr     Resolved address.
 * @param[in] addr_len Length of @p addr.
 * @param[in] timeout_ms Connect deadline in milliseconds (0 for a default).
 *
 * @return 0 if the connect was submitted, negative errno on failure.
 * @thread_safety Loop thread only.
 */
int conn_connect(conn_t *c, const struct sockaddr *addr, socklen_t addr_len, uint64_t timeout_ms);

/**
 * @brief Queue a request once the connection is ready (state CONN_HTTP_WRITE).
 *
 * @param[in] c   Connection.
 * @param[in] buf Request bytes (copied into the connection's write buffer).
 * @param[in] len Request length.
 *
 * @return 0 if queued, -EAGAIN if the connection is not ready yet, other negative
 *         errno on failure.
 * @ownership @p buf is copied; the caller may free it after return.
 * @thread_safety Loop thread only.
 */
int conn_send_request(conn_t *c, const char *buf, size_t len);

/**
 * @brief Return the parsed response, valid once the callback fired with status 0.
 *
 * @param[in] c Connection.
 *
 * @return Pointer to the response, or NULL.
 * @ownership Owned by the connection; valid until conn_destroy().
 * @thread_safety Loop thread only.
 */
const http_resp_t *conn_response(const conn_t *c);

/**
 * @brief Return the current state.
 *
 * @param[in] c Connection.
 *
 * @return Current state.
 * @thread_safety Loop thread only.
 */
conn_state_t conn_state(const conn_t *c);

/**
 * @brief Report whether the connection may be reused for another request.
 *
 * True only after a completed response that advertised keep-alive and delivered
 * no bytes beyond the end of that response (pipelined data would be lost on
 * reuse). SPEC section 4.2 keep-alive.
 *
 * @param[in] c Connection.
 *
 * @return 1 if the connection can serve another request, else 0.
 * @thread_safety Loop thread only.
 */
int conn_keep_alive(const conn_t *c);

/**
 * @brief Prepare a completed connection for another request on the same socket.
 *
 * Frees the previous response, resets the parser and write buffer, and returns
 * the connection to CONN_HTTP_WRITE with its fd and TLS session intact. The
 * caller then issues the next request with conn_send_request().
 *
 * @param[in,out] c Connection in state CONN_DONE.
 *
 * @return 0 on success, -EINVAL if not in CONN_DONE, -EPROTO if the connection
 *         may not be reused (see conn_keep_alive()).
 * @thread_safety Loop thread only.
 */
int conn_reuse_begin(conn_t *c);

/**
 * @brief Return the socket fd, or -1 if not open.
 *
 * @param[in] c Connection.
 *
 * @return fd or -1.
 * @thread_safety Loop thread only.
 */
int conn_fd(const conn_t *c);

/**
 * @brief Destroy the connection, closing the socket if still open.
 *
 * @param[in] c Connection (may be NULL).
 * @thread_safety Loop thread only.
 */
void conn_destroy(conn_t *c);

#endif /* TG_NET_CONN_H */
