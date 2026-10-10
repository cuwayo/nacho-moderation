/**
 * @file rate_limit.h
 * @brief Outbound Bot API rate limiter: token buckets with an injectable clock.
 *
 * SPEC section 4.2. Three bucket classes guard outbound requests:
 *  - `BUCKET_GLOBAL`: 25 requests/second, shared by every call;
 *  - `BUCKET_PER_CHAT`: 15 requests/minute per chat (messages);
 *  - `BUCKET_BULK`: 1 request/second per chat (bulk operations).
 *
 * A request is admitted only when every bucket that applies to it has a token;
 * otherwise the caller queues it and retries later (excess requests are never
 * dropped silently). Tokens are tracked in micro-token units (1e6 = 1 token) so
 * the refill is exact integer math under the injectable @ref clock_ops_t.
 *
 * On HTTP 429 the caller pauses the affected bucket for the server-supplied
 * `retry_after` interval with @ref rate_limiter_pause; other chats are unaffected.
 *
 * The limiter is pure logic: it holds no queue and performs no I/O. Time is read
 * only through the injected clock, never `time()`/`clock_gettime()` (A3).
 *
 * @ingroup api
 */
#ifndef TG_RATE_LIMIT_H
#define TG_RATE_LIMIT_H

#include <stdint.h>

#include "tables.h"
#include "tg/util.h"

/** @brief Opaque rate limiter. */
typedef struct rate_limiter rate_limiter_t;

/**
 * @brief Create a rate limiter with every bucket initially full.
 *
 * @param[in] clock Injectable clock used to timestamp tokens; must outlive the
 *                  limiter (never NULL).
 *
 * @return A new limiter, or NULL on failure.
 * @ownership Caller must call rate_limiter_destroy().
 * @thread_safety Loop thread only.
 */
rate_limiter_t *rate_limiter_create(const clock_ops_t *clock);

/**
 * @brief Destroy the limiter and free its per-chat buckets.
 *
 * @param[in] lr Limiter (may be NULL).
 * @thread_safety Loop thread only.
 */
void rate_limiter_destroy(rate_limiter_t *lr);

/**
 * @brief Try to take one token for a request in @p bucket for @p chat_id.
 *
 * Consumes a token from every bucket that applies to the request only when all
 * of them admit (the global bucket, plus the per-chat message or bulk bucket for
 * the corresponding classes).
 *
 * @param[in] lr      Limiter.
 * @param[in] bucket  Request bucket class.
 * @param[in] chat_id Target chat (ignored for `BUCKET_GLOBAL`).
 *
 * @return 0 if a token was taken, -EAGAIN if the request must be retried later,
 *         -EINVAL on a bad argument.
 * @thread_safety Loop thread only.
 */
int rate_limiter_acquire(rate_limiter_t *lr, bucket_t bucket, int64_t chat_id);

/**
 * @brief Pause a bucket for @p retry_after_ms (HTTP 429 `parameters.retry_after`).
 *
 * The pause applies to the bucket class the request used, so pausing a message
 * send in one chat does not stall traffic to other chats.
 *
 * @param[in] lr             Limiter.
 * @param[in] bucket         Request bucket class.
 * @param[in] chat_id        Target chat (ignored for `BUCKET_GLOBAL`).
 * @param[in] retry_after_ms Server-requested pause in milliseconds (0 for none).
 * @thread_safety Loop thread only.
 */
void rate_limiter_pause(rate_limiter_t *lr, bucket_t bucket, int64_t chat_id,
                        uint64_t retry_after_ms);

/**
 * @brief Earliest absolute time (ms) at which a request could be admitted.
 *
 * Lets the caller arm a single io_uring timeout for the next time the queue
 * could make progress. Returns the current clock time when a token is available
 * immediately.
 *
 * @param[in] lr      Limiter.
 * @param[in] bucket  Request bucket class.
 * @param[in] chat_id Target chat (ignored for `BUCKET_GLOBAL`).
 *
 * @return Absolute millisecond timestamp; the caller waits until then.
 * @thread_safety Loop thread only.
 */
uint64_t rate_limiter_ready_at_ms(const rate_limiter_t *lr, bucket_t bucket, int64_t chat_id);

/**
 * @brief Exponential backoff delay for a retry (SPEC section 4.2).
 *
 * Schedule: 1s, 2s, 4s, 8s, 16s, capped at 30s for later retries.
 *
 * @param[in] retry_index Number of retries already performed (0 for the first).
 *
 * @return Delay in milliseconds.
 * @thread_safety Safe (no shared state).
 */
uint64_t rate_limit_backoff_ms(unsigned retry_index);

#endif /* TG_RATE_LIMIT_H */
