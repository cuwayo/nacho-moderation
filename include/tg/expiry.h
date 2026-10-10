/**
 * @file expiry.h
 * @brief Periodic lifter for expired temp bans and mutes (SPEC section 5.6).
 *
 * Temp bans and mutes are stored with an absolute `actions.expires_at`. A periodic
 * timer (every 30 s by default) asks the DB worker for the rows that are now due and
 * lifts each one: a `ban` becomes `unbanChatMember`, a `mute` becomes
 * `restrictChatMember` with the common group permissions restored. The row is then
 * marked lifted so it is never applied twice, and because `expires_at` is a
 * wall-clock timestamp the flow survives a process restart (SPEC section 5.6).
 *
 * The sweeper never blocks the loop: the DB query runs on the DB worker thread and
 * the lift calls go through the API pool. Time is read only through the injected
 * clocks (SPEC-ADDENDUM-TOOLCHAIN A3).
 *
 * @ingroup commands
 */
#ifndef TG_EXPIRY_H
#define TG_EXPIRY_H

#include <stdint.h>

#include "tg/db.h"
#include "tg/loop.h"
#include "tg/tg_api.h"
#include "tg/util.h"

/** @brief Default sweep interval in milliseconds (SPEC section 5.6: 30 s). */
#define EXPIRY_INTERVAL_MS 30000u

/** @brief The expiry sweeper (opaque; see expiry.c). */
typedef struct expiry expiry_t;

/** @brief Sweeper configuration. All pointers must outlive the sweeper. */
typedef struct {
    const loop_ops_t *ops;        /**< Loop vtable (arms the periodic timer). */
    const db_ops_t *db;           /**< DB seam (expiry query + lift). */
    const tg_api_ops_t *api;      /**< API seam (unban / restore permissions). */
    const wall_clock_ops_t *wall; /**< Wall clock for the expiry cutoff. */
    uint64_t interval_ms;         /**< Sweep interval (0 = @ref EXPIRY_INTERVAL_MS). */
} expiry_config_t;

/**
 * @brief Create the sweeper (no timer is armed until @ref expiry_start).
 *
 * @param[in] cfg Configuration (copied; pointers must outlive the sweeper).
 *
 * @return A new sweeper, or NULL on failure.
 * @ownership Caller must call expiry_destroy().
 * @thread_safety Loop thread only.
 */
expiry_t *expiry_create(const expiry_config_t *cfg);

/**
 * @brief Arm the periodic sweep timer.
 *
 * @param[in] e Sweeper.
 *
 * @return 0 if armed, negative errno on failure.
 * @thread_safety Loop thread only.
 */
int expiry_start(expiry_t *e);

/**
 * @brief Stop the sweeper (does not cancel an in-flight sweep).
 *
 * @param[in] e Sweeper.
 * @thread_safety Loop thread only.
 */
void expiry_stop(expiry_t *e);

/**
 * @brief Destroy the sweeper.
 *
 * @param[in] e Sweeper (may be NULL).
 * @thread_safety Loop thread only.
 */
void expiry_destroy(expiry_t *e);

/**
 * @brief Run one sweep immediately (used by tests; the timer calls this too).
 *
 * Submits the expiry query through the DB seam; the lifts happen in its callback.
 *
 * @param[in] e Sweeper.
 *
 * @return 0 if the query was queued, negative errno on failure.
 * @thread_safety Loop thread only.
 */
int expiry_sweep_now(expiry_t *e);

#endif /* TG_EXPIRY_H */
