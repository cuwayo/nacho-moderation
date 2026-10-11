/**
 * @file webhook.h
 * @brief Inbound webhook server for update delivery (SPEC section 4.1, webhook mode).
 *
 * In webhook mode the bot listens on plain HTTP behind a reverse proxy that
 * terminates TLS. The accept loop uses @c io_uring_prep_accept; each accepted
 * connection is read with @c io_uring_prep_recv, parsed by the pure
 * @ref http_req_t state machine, checked against the configured secret token
 * (`X-Telegram-Bot-Api-Secret-Token`, SPEC section 10) and handed to the router as
 * one Update. The response is written with @c io_uring_prep_send and the
 * connection is closed (`Connection: close`).
 *
 * The server never blocks the loop: the listening socket is created and bound once
 * at startup (before the loop runs, the same pattern as DNS resolution), and all
 * subsequent I/O is io_uring. The module uses the injectable @ref loop_ops_t and
 * @ref net_ops_t seams so it can be driven with a fake ring in unit tests.
 *
 * @ingroup api
 */
#ifndef TG_WEBHOOK_H
#define TG_WEBHOOK_H

#include <stddef.h>

#include "tg/http.h"
#include "tg/loop.h"
#include "tg/net_conn.h"

/** @brief Default maximum number of concurrent inbound connections. */
#define WEBHOOK_MAX_CONNS 8u

/** @brief Default maximum inbound request body size (bytes), per SPEC section 9. */
#define WEBHOOK_MAX_BODY (1024UL * 1024UL)

/**
 * @brief Called with the raw body of one verified webhook update.
 *
 * The router adapter parses the body as a single Update and dispatches it. The
 * pointer is borrowed for the duration of the call only.
 */
typedef void (*webhook_update_cb)(const char *body, size_t len, void *ctx);

/** @brief Configuration for the webhook server. All pointers must outlive it. */
typedef struct {
    const loop_ops_t *ops;       /**< Loop vtable. */
    const net_ops_t *net;        /**< Socket vtable. */
    const char *listen_addr;     /**< "host:port" to bind (e.g. "127.0.0.1:8080"). */
    const char *secret;          /**< Expected secret token ("" disables the check). */
    size_t max_body;             /**< Body size cap (0 for @ref WEBHOOK_MAX_BODY). */
    unsigned max_conns;          /**< Connection slots (0 for @ref WEBHOOK_MAX_CONNS). */
    webhook_update_cb on_update; /**< Update callback (never NULL). */
    void *ctx;                   /**< Opaque pointer passed to @p on_update. */
} webhook_config_t;

/** @brief The webhook server (opaque; see webhook.c). */
typedef struct webhook webhook_t;

/**
 * @brief Create the webhook server (no socket is opened until @ref webhook_start).
 *
 * @param[in] cfg Configuration (copied; pointers must outlive the server).
 *
 * @return A new server, or NULL on failure.
 * @ownership Caller must call webhook_destroy().
 * @thread_safety Loop thread only.
 */
webhook_t *webhook_create(const webhook_config_t *cfg);

/**
 * @brief Bind, listen and arm the first accept.
 *
 * The listening socket is created, bound and put in listen mode synchronously
 * (startup, before the loop runs); the accept itself is then submitted through the
 * ring. After this returns, the server never blocks.
 *
 * @param[in] w Server.
 *
 * @return 0 on success, negative errno on failure.
 * @thread_safety Loop thread only (call once, before entering the loop).
 */
int webhook_start(webhook_t *w);

/**
 * @brief Return the listening socket fd, or -1 if not started.
 *
 * @param[in] w Server.
 *
 * @return The listen fd or -1.
 * @thread_safety Loop thread only.
 */
int webhook_listen_fd(const webhook_t *w);

/**
 * @brief Number of connections with work still in flight.
 *
 * @param[in] w Server.
 *
 * @return Active connection count (for the graceful-shutdown drain).
 * @thread_safety Loop thread only.
 */
unsigned webhook_active_conns(const webhook_t *w);

/**
 * @brief Stop accepting new connections (in-flight ones are left to finish).
 *
 * @param[in] w Server.
 * @thread_safety Loop thread only.
 */
void webhook_stop(webhook_t *w);

/**
 * @brief Close every connection and release the server.
 *
 * @param[in] w Server (may be NULL).
 * @thread_safety Loop thread only.
 */
void webhook_destroy(webhook_t *w);

/**
 * @brief Verify a request's secret token against the expected value (pure).
 *
 * Uses a constant-time comparison so the token cannot be recovered by timing.
 *
 * @param[in] expected Configured secret. An empty or NULL value rejects every
 *                     request (webhook mode refuses to start without a secret).
 * @param[in] got      Header value from the request ("" if absent).
 *
 * @return 1 if the request is authorized, 0 otherwise.
 * @thread_safety Safe (no shared state).
 */
int webhook_verify_secret(const char *expected, const char *got);

/**
 * @brief Decide whether a parsed request should be accepted (pure).
 *
 * A request is accepted only when the method is POST and the secret matches. The
 * status to answer with is reported in @p out_status (200, 403 or 405).
 *
 * @param[in]  req             Parsed request.
 * @param[in]  expected_secret Configured secret. An empty or NULL value rejects
 *                             every request (403).
 * @param[out] out_status      Receives the HTTP status to answer with.
 *
 * @return 1 if the request should be routed, 0 otherwise.
 * @thread_safety Safe (no shared state).
 */
int webhook_request_ok(const http_req_t *req, const char *expected_secret, int *out_status);

#endif /* TG_WEBHOOK_H */
