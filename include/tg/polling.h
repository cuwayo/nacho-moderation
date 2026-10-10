/**
 * @file polling.h
 * @brief getUpdates long-polling loop (SPEC section 4.1, polling mode).
 *
 * Keeps one long-poll in flight at a time on the API client's dedicated polling
 * connection, tracks the update offset (`last_update_id + 1`) and re-arms
 * immediately after each reply. In M2 the update bodies are counted and the
 * offset advanced; the router that consumes them arrives in M3, so the callback
 * receives only the batch size and the new offset.
 *
 * Failures never lose updates: the offset is advanced only on a successful reply,
 * and the next poll is re-armed on any outcome.
 *
 * @ingroup polling
 */
#ifndef TG_POLLING_H
#define TG_POLLING_H

#include <stddef.h>
#include <stdint.h>

#include "tg/tg_api.h"
#include "tg/util.h"

/** @brief Long-poll result passed to the batch callback. */
typedef struct {
    unsigned count;   /**< Updates received in this batch (0 for an empty reply). */
    int64_t offset;   /**< Offset used for the next poll. */
    const char *body; /**< Raw reply body (valid only during the callback). */
    size_t body_len;  /**< Length of @p body in bytes. */
} poll_batch_t;

/** @brief Called after each successful poll with the batch summary. */
typedef void (*poll_batch_cb)(const poll_batch_t *batch, void *ctx);

/** @brief The getUpdates poller (opaque; see polling.c). */
typedef struct poller poller_t;

/** @brief Poller configuration. All pointers must outlive the poller. */
typedef struct {
    const tg_api_ops_t *ops;  /**< API client seam. */
    const clock_ops_t *clock; /**< Injectable clock (deadlines). */
    unsigned timeout_s;       /**< Long-poll timeout in seconds (SPEC: 30). */
    uint64_t extra_ms;        /**< Deadline margin added to the poll timeout. */
    poll_batch_cb on_batch;   /**< Batch callback (may be NULL). */
    void *ctx;                /**< Opaque pointer passed to @p on_batch. */
} poller_config_t;

/**
 * @brief Create a poller (no request is issued until @ref poller_start).
 *
 * @param[in] cfg Configuration (copied; pointers must outlive the poller).
 *
 * @return A new poller, or NULL on failure.
 * @ownership Caller must call poller_destroy().
 * @thread_safety Loop thread only.
 */
poller_t *poller_create(const poller_config_t *cfg);

/**
 * @brief Start polling (issues the first getUpdates).
 *
 * @param[in] poller Poller.
 *
 * @return 0 if the first poll was queued, negative errno on failure.
 * @thread_safety Loop thread only.
 */
int poller_start(poller_t *poller);

/**
 * @brief Stop polling (does not cancel an in-flight request).
 *
 * @param[in] poller Poller.
 * @thread_safety Loop thread only.
 */
void poller_stop(poller_t *poller);

/**
 * @brief Return the current update offset.
 *
 * @param[in] poller Poller.
 *
 * @return The offset used for the next poll.
 * @thread_safety Loop thread only.
 */
int64_t poller_offset(const poller_t *poller);

/**
 * @brief Destroy the poller.
 *
 * @param[in] poller Poller (may be NULL).
 * @thread_safety Loop thread only.
 */
void poller_destroy(poller_t *poller);

/**
 * @brief Advance @p offset past the highest update_id in a getUpdates reply.
 *
 * Pure helper (no I/O) so the offset logic is unit-testable. Only a well-formed
 * `{"ok":true,"result":[...]}` reply advances the offset; a malformed reply or an
 * error envelope leaves it unchanged and returns -ERR_PARSE.
 *
 * @param[in,out] offset Current offset; set to `max(update_id)+1` on success.
 * @param[in]     json   Reply body.
 * @param[in]     len    Length.
 * @param[out]    count  Receives the number of updates in the batch.
 *
 * @return 0 on success (including an empty batch), -ERR_PARSE on a malformed
 *         reply or a missing result array, -EINVAL on bad arguments.
 * @thread_safety Safe (no shared state).
 */
int poller_advance_offset(int64_t *offset, const char *json, size_t len, unsigned *count);

#endif /* TG_POLLING_H */
