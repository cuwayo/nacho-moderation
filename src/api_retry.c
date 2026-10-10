/**
 * @file api_retry.c
 * @brief Retry decision logic for Bot API calls (SPEC section 4.2).
 *
 * @ingroup api
 */
#include "tg/api_retry.h"

#include <stdint.h>

#include "api_methods.h"
#include "tg/errors.h"

api_retry_action_t api_retry_decide(method_id_t method, int outcome, unsigned retries_done,
                                    uint64_t now_ms, uint64_t deadline_ms)
{
    /* A deadline in the past stops every retry: the request has run out of time. */
    if (deadline_ms != 0 && now_ms >= deadline_ms) {
        return API_RETRY_NO;
    }

    /* HTTP 429: the bucket was paused by the caller; requeue immediately and let
     * the rate limiter hold it until retry_after elapses. */
    if (outcome == -ERR_RATE_LIMIT) {
        return API_RETRY_NOW;
    }

    /* Network failures and unexpected 5xx are retried only when the method is
     * idempotent, with exponential backoff, up to API_MAX_RETRIES. */
    if (outcome == -ERR_NET || outcome == -ERR_HTTP) {
        if (api_method_is_idempotent(method) && retries_done < API_MAX_RETRIES) {
            return API_RETRY_BACKOFF;
        }
        return API_RETRY_NO;
    }

    /* Everything else (4xx, ok=false, parse, protocol, TLS) is terminal. */
    return API_RETRY_NO;
}
