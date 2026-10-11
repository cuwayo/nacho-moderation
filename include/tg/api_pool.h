/**
 * @file api_pool.h
 * @brief Pooled, keep-alive Bot API client with rate limiting and retries.
 *
 * SPEC section 4.2. The pool owns @c pool_size connections to the Bot API, a FIFO
 * of pending requests, the shared rate limiter and a single dispatch/retry timer.
 * It is driven by the io_uring loop: call @ref api_pool_pump once per loop
 * iteration; completions arrive through the connections' callbacks.
 *
 * Design rules:
 *  - Requests are never dropped silently: when the rate limiter or a backoff
 *    holds a request it stays queued and the pool arms an io_uring timeout.
 *  - Sends are issued only from @ref api_pool_pump (never inside a completion
 *    callback), which keeps the pool free of re-entrancy.
 *  - Slot 0 is reserved for polling so the long-poll has a dedicated connection
 *    (SPEC section 4.1); the remaining slots serve shared traffic.
 *  - The pool never blocks: the address is resolved by the caller before the pool
 *    is created (SPEC section 4.4, "at startup").
 *
 * @ingroup api
 */
#ifndef TG_API_POOL_H
#define TG_API_POOL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "api_methods.h"
#include "tg/loop.h"
#include "tg/net_conn.h"
#include "tg/rate_limit.h"
#include "tg/util.h"

/** @brief Maximum number of requests queued before @ref api_pool_submit fails. */
#define API_POOL_QUEUE_MAX 4096u

/** @brief Maximum dispatch attempts for a single request (bounds 429 livelock). */
#define API_MAX_ATTEMPTS 20u

/** @brief Which connection class a request uses. */
typedef enum {
    POOL_CLASS_SHARED = 0, /**< A pooled connection (slots 1..N-1). */
    POOL_CLASS_POLLING     /**< The dedicated polling connection (slot 0). */
} pool_class_t;

/**
 * @brief Parsed Bot API response passed to a request callback.
 *
 * @p body points into the connection's response buffer and is valid only for the
 * duration of the callback; copy anything that must outlive it.
 */
typedef struct {
    int status;            /**< 0 on success, negative @ref err_class_t otherwise. */
    int http_status;       /**< HTTP status code (0 if no response arrived). */
    int ok;                /**< 1 if the envelope said `ok=true`. */
    int error_code;        /**< Telegram `error_code` (0 if none). */
    char description[256]; /**< Telegram `description` / error text. */
    uint64_t retry_after;  /**< 429 `parameters.retry_after` in seconds (0 if none). */
    const char *body;      /**< Raw response body (pool-owned; callback lifetime). */
    size_t body_len;       /**< Length of @p body in bytes. */
} api_result_t;

/** @brief Completion callback; runs on the loop thread. */
typedef void (*api_pool_cb)(const api_result_t *res, void *ctx);

/** @brief Pool configuration. All pointers must outlive the pool. */
typedef struct {
    const char *token;           /**< Bot token (never logged). */
    const char *user_agent;      /**< User-Agent header value. */
    const char *host;            /**< Host for the Host header and TLS SNI. */
    int use_tls;                 /**< 1 to wrap connections in TLS, 0 for plain HTTP. */
    const struct sockaddr *addr; /**< Pre-resolved Bot API address (copied). */
    socklen_t addr_len;          /**< Length of @p addr. */
    unsigned pool_size;          /**< Number of connections (>= 1). */
    uint64_t request_timeout_ms; /**< Connect/request deadline in milliseconds. */
    const loop_ops_t *ops;       /**< Loop vtable. */
    const net_ops_t *net;        /**< Socket vtable. */
    const clock_ops_t *clock;    /**< Injectable clock. */
    rate_limiter_t *limiter;     /**< Shared rate limiter (not owned). */
} api_pool_config_t;

/** @brief The connection pool (opaque; see api_pool.c). */
typedef struct api_pool api_pool_t;

/**
 * @brief Create an API pool (no I/O is started until the first submit/pump).
 *
 * @param[in] cfg Configuration (copied; pointers must outlive the pool).
 *
 * @return A new pool, or NULL on failure.
 * @ownership Caller must call api_pool_destroy().
 * @thread_safety Loop thread only.
 */
api_pool_t *api_pool_create(const api_pool_config_t *cfg);

/**
 * @brief Queue a Bot API call.
 *
 * The body is copied. The bucket class and idempotency flag come from the method
 * table (src/api_methods.def). The callback runs exactly once, on success or
 * terminal failure; it is not called when this function returns an error.
 *
 * @param[in] pool        Pool.
 * @param[in] method      API method identifier.
 * @param[in] body        JSON request body (may be NULL when @p body_len is 0).
 * @param[in] body_len    Length of @p body.
 * @param[in] chat_id     Target chat for per-chat rate limiting (0 if none).
 * @param[in] cls         Connection class (@ref pool_class_t).
 * @param[in] deadline_ms Absolute deadline; 0 means none.
 * @param[in] cb          Completion callback (never NULL).
 * @param[in] ctx         Opaque pointer passed to @p cb.
 *
 * @return 0 if queued, -EINVAL on bad arguments, -ENOSPC if the queue is full.
 * @ownership @p ctx is owned by the caller and must stay valid until @p cb runs.
 * @thread_safety Loop thread only.
 */
int api_pool_submit(api_pool_t *pool, method_id_t method, const char *body, size_t body_len,
                    int64_t chat_id, pool_class_t cls, uint64_t deadline_ms, api_pool_cb cb,
                    void *ctx);

/**
 * @brief Dispatch queued requests and arm the retry/rate timer. Call once per loop
 *        iteration.
 *
 * @param[in] pool Pool.
 * @thread_safety Loop thread only.
 */
void api_pool_pump(api_pool_t *pool);

/**
 * @brief Destroy the pool, its connections and any queued requests.
 *
 * Queued requests are abandoned without invoking their callbacks. Graceful
 * shutdown drains outstanding requests first (see @ref api_pool_outstanding and
 * the drain in `main.c`, SPEC section 9), so this is only reached once the pool
 * is idle or the drain budget has elapsed.
 *
 * @param[in] pool Pool (may be NULL).
 * @thread_safety Loop thread only.
 */
void api_pool_destroy(api_pool_t *pool);

/**
 * @brief Number of requests currently queued (not yet in flight).
 *
 * @param[in] pool Pool.
 *
 * @return Queue depth.
 * @thread_safety Loop thread only.
 */
size_t api_pool_queue_depth(const api_pool_t *pool);

/**
 * @brief Number of requests still outstanding (queued plus in flight).
 *
 * Used by the graceful-shutdown drain (SPEC section 9) to decide when every
 * outbound call has finished.
 *
 * @param[in] pool Pool.
 *
 * @return Outstanding request count.
 * @thread_safety Loop thread only.
 */
size_t api_pool_outstanding(const api_pool_t *pool);

/**
 * @brief Number of outstanding requests of one connection class.
 *
 * The graceful-shutdown drain uses this to ignore the dedicated polling
 * connection: a long-poll is the update source, which shutdown stops rather than
 * drains (SPEC section 9).
 *
 * @param[in] pool Pool.
 * @param[in] cls  Connection class to count.
 *
 * @return Outstanding request count of that class.
 * @thread_safety Loop thread only.
 */
size_t api_pool_outstanding_class(const api_pool_t *pool, pool_class_t cls);

#endif /* TG_API_POOL_H */
