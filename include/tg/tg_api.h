/**
 * @file tg_api.h
 * @brief Telegram Bot API client: pooled keep-alive connections, rate limiting,
 *        retries, and the injectable seam used by the poller.
 *
 * The client owns an @ref api_pool_t. Callers submit a method call with
 * @ref tg_api_request (or a convenience wrapper); the callback runs on the loop
 * thread once the call succeeds or fails terminally. JSON helpers are exposed for
 * unit testing without a network.
 *
 * @ingroup api
 */
#ifndef TG_TG_API_H
#define TG_TG_API_H

#include <stddef.h>
#include <stdint.h>

#include "api_methods.h"
#include "tg/api_pool.h"
#include "tg/loop.h"
#include "tg/net_conn.h"
#include "tg/util.h"

/** @brief Parsed bot identity from getMe. */
typedef struct {
    int64_t id;           /**< Bot user id. */
    int is_bot;           /**< Always 1 for a bot. */
    int ok;               /**< 1 if the response envelope said ok=true. */
    char first_name[128]; /**< Bot first name. */
    char username[64];    /**< Bot username (without the leading at-sign). */
} tg_me_t;

/** @brief Result callback for a single API call (see @ref api_result_t). */
typedef void (*tg_api_cb)(const api_result_t *res, void *ctx);

/** @brief Injectable API client seam (SPEC-ADDENDUM-TOOLCHAIN A3). */
typedef struct {
    /**
     * @brief Queue one method call.
     *
     * @return 0 if queued, negative errno on failure.
     */
    int (*request)(void *ctx, method_id_t method, const char *body, size_t body_len,
                   int64_t chat_id, uint64_t deadline_ms, tg_api_cb cb, void *user);
    void *ctx; /**< Implementation state (an @ref tg_api_t). */
} tg_api_ops_t;

/** @brief Configuration for an API client. */
typedef struct {
    const char *token;           /**< Bot token (never logged). */
    const char *base_url;        /**< Base URL, e.g. "https://api.telegram.org". */
    const char *user_agent;      /**< User-Agent header. */
    unsigned pool_size;          /**< Connections in the pool (>= 1). */
    uint64_t request_timeout_ms; /**< Per-request deadline in milliseconds. */
} tg_api_config_t;

/** @brief The Bot API client (opaque; see tg_api.c). */
typedef struct tg_api tg_api_t;

/**
 * @brief Create an API client.
 *
 * @param[in] cfg   Configuration (token, base URL, user agent, pool size).
 * @param[in] ops   Loop vtable that drives connections.
 * @param[in] net   Socket vtable.
 * @param[in] clock Clock vtable.
 *
 * @return A new client, or NULL on failure.
 * @ownership Caller must call tg_api_destroy().
 * @thread_safety Loop thread only.
 */
tg_api_t *tg_api_create(const tg_api_config_t *cfg, const loop_ops_t *ops, const net_ops_t *net,
                        const clock_ops_t *clock);

/**
 * @brief Return the injectable seam for this client.
 *
 * @param[in] api Client.
 *
 * @return Pointer to the seam (owned by the client), or NULL.
 * @ownership Owned by @p api; do not free.
 * @thread_safety Loop thread only.
 */
const tg_api_ops_t *tg_api_ops(tg_api_t *api);

/**
 * @brief Queue a Bot API call (see @ref api_pool_submit).
 *
 * @param[in] api      Client.
 * @param[in] method   API method identifier.
 * @param[in] body     JSON request body (may be NULL when @p body_len is 0).
 * @param[in] body_len Length of @p body.
 * @param[in] chat_id  Target chat for per-chat rate limiting (0 if none).
 * @param[in] deadline_ms Absolute deadline (0 for none).
 * @param[in] cb       Completion callback (never NULL).
 * @param[in] ctx      Opaque pointer passed to @p cb.
 *
 * @return 0 if queued, negative errno on failure.
 * @thread_safety Loop thread only.
 */
int tg_api_request(tg_api_t *api, method_id_t method, const char *body, size_t body_len,
                   int64_t chat_id, uint64_t deadline_ms, tg_api_cb cb, void *ctx);

/**
 * @brief Queue a getMe request.
 *
 * @param[in] api Client.
 * @param[in] cb  Completion callback.
 * @param[in] ctx Opaque pointer passed to @p cb.
 *
 * @return 0 if queued, negative errno on failure.
 * @thread_safety Loop thread only.
 */
int tg_api_get_me(tg_api_t *api, tg_api_cb cb, void *ctx);

/**
 * @brief Queue a getUpdates long-poll.
 *
 * @param[in] api       Client.
 * @param[in] offset    Update offset (0 to start from the beginning).
 * @param[in] timeout_s Long-poll timeout in seconds.
 * @param[in] deadline_ms Absolute deadline (must exceed @p timeout_s).
 * @param[in] cb        Completion callback.
 * @param[in] ctx       Opaque pointer passed to @p cb.
 *
 * @return 0 if queued, negative errno on failure.
 * @thread_safety Loop thread only.
 */
int tg_api_get_updates(tg_api_t *api, int64_t offset, unsigned timeout_s, uint64_t deadline_ms,
                       tg_api_cb cb, void *ctx);

/**
 * @brief Build the JSON body for a getUpdates long-poll.
 *
 * Emits `{"offset":N,"timeout":T,"allowed_updates":[...]}` with the update types
 * from SPEC section 4.1. Shared by @ref tg_api_get_updates and the poller.
 *
 * @param[out] out       Output buffer.
 * @param[in]  cap       Capacity of @p out.
 * @param[in]  offset    Update offset (0 to start from the beginning).
 * @param[in]  timeout_s Long-poll timeout in seconds.
 * @param[out] outlen    Receives the body length (excluding NUL).
 *
 * @return 0 on success, -ENOSPC if @p out is too small, -EINVAL on bad arguments.
 * @thread_safety Safe (no shared state).
 */
int tg_api_build_get_updates_body(char *out, size_t cap, int64_t offset, unsigned timeout_s,
                                  size_t *outlen);

/**
 * @brief Advance the pool: dispatch queued calls and arm the retry timer.
 *
 * @param[in] api Client.
 * @thread_safety Loop thread only.
 */
void tg_api_pump(tg_api_t *api);

/**
 * @brief Destroy the client and its pool.
 *
 * @param[in] api Client (may be NULL).
 * @thread_safety Loop thread only.
 */
void tg_api_destroy(tg_api_t *api);

/**
 * @brief Parse a Bot API response envelope into @p out.
 *
 * Extracts `ok`, `error_code`, `description` and `parameters.retry_after`. Does
 * not touch @p out->body, which the caller sets separately.
 *
 * @param[in]  json JSON bytes.
 * @param[in]  len  Length.
 * @param[out] out  Result to fill (body fields are left unchanged).
 *
 * @return 0 on success, -ERR_PARSE on malformed JSON, -EINVAL on bad arguments.
 * @thread_safety Safe (no shared state).
 */
int tg_api_parse_result(const char *json, size_t len, api_result_t *out);

/**
 * @brief Parse a getMe JSON body into @p out.
 *
 * Exposed for unit testing without a network. Accepts the standard
 * `{"ok":true,"result":{...}}` shape.
 *
 * @param[in]  json JSON bytes.
 * @param[in]  len  Length.
 * @param[out] out  Parsed identity.
 *
 * @return 0 on success, -ERR_PARSE on malformed JSON or a missing result, -EINVAL
 *         on bad arguments.
 * @thread_safety Safe (no shared state).
 */
int tg_api_parse_me(const char *json, size_t len, tg_me_t *out);

/**
 * @brief Parse a Bot API base URL into scheme, host and port.
 *
 * Exposed so the runtime can resolve the host before creating the pool.
 *
 * @param[in]  url      Base URL (e.g. "https://api.telegram.org").
 * @param[out] scheme   Receives "http" or "https".
 * @param[in]  scheme_cap Capacity of @p scheme.
 * @param[out] host     Receives the host name.
 * @param[in]  host_cap Capacity of @p host.
 * @param[out] port     Receives the port string.
 * @param[in]  port_cap Capacity of @p port.
 *
 * @return 0 on success, -EINVAL on a malformed URL or a too-small buffer.
 * @thread_safety Safe (no shared state).
 */
int tg_api_parse_base_url(const char *url, char *scheme, size_t scheme_cap, char *host,
                          size_t host_cap, char *port, size_t port_cap);

#endif /* TG_TG_API_H */
