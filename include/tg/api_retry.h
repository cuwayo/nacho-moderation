/**
 * @file api_retry.h
 * @brief Retry policy for Bot API calls (SPEC section 4.2).
 *
 * Pure decision logic: given a method and its outcome, decide whether to retry,
 * and if so whether to wait out an exponential backoff first. The caller (the
 * API pool) owns the io_uring timer; this module only reasons about the policy,
 * the idempotency flag from src/api_methods.def, the retry count and the deadline.
 *
 * Rules:
 *  - HTTP 429 (`-ERR_RATE_LIMIT`): retry after the bucket pause, bounded by the
 *    deadline (the pause is applied by the rate limiter, not here).
 *  - 5xx / network error (`-ERR_HTTP`, `-ERR_NET`): retry with backoff only for
 *    idempotent methods, at most @ref API_MAX_RETRIES times.
 *  - 4xx other than 429, `ok=false` errors, parse and protocol failures: never.
 *
 * @ingroup api
 */
#ifndef TG_API_RETRY_H
#define TG_API_RETRY_H

#include <stdint.h>

#include "api_methods.h"

/** @brief Maximum retries for idempotent calls after 5xx / network errors. */
#define API_MAX_RETRIES 5u

/** @brief What the caller should do after an outcome. */
typedef enum {
    API_RETRY_NO = 0, /**< Do not retry; fail the request callback. */
    API_RETRY_NOW,    /**< Requeue immediately (429 pause already applied). */
    API_RETRY_BACKOFF /**< Requeue after rate_limit_backoff_ms(retries_done). */
} api_retry_action_t;

/**
 * @brief Decide the retry action for a completed request.
 *
 * @param[in] method        Method identifier (its idempotency flag is consulted).
 * @param[in] outcome       Negative @ref err_class_t value classifying the result
 *                          (e.g. -ERR_NET, -ERR_HTTP, -ERR_RATE_LIMIT, -ERR_PARSE).
 * @param[in] retries_done  Number of retries already performed (0 on first try).
 * @param[in] now_ms        Current time from the injected clock.
 * @param[in] deadline_ms   Absolute deadline; 0 means no deadline.
 *
 * @return The action to take.
 * @thread_safety Safe (no shared state).
 */
api_retry_action_t api_retry_decide(method_id_t method, int outcome, unsigned retries_done,
                                    uint64_t now_ms, uint64_t deadline_ms);

#endif /* TG_API_RETRY_H */
