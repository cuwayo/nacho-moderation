/**
 * @file settings.h
 * @brief Per-chat moderation policy: cached reads over the DB worker (SPEC 4.4, 5.2).
 *
 * The `chats` row is the source of truth for a chat's policy (warn limit and
 * action, welcome text/enabled, rules text, log channel). Reading it on every
 * command would cost a DB round-trip, so the settings module keeps a small
 * in-memory TTL cache in front of the DB worker, mirroring the administrator
 * cache in @ref perms_admins_get.
 *
 *  - a cache hit resolves synchronously (the callback runs before returning);
 *  - a miss submits `DB_JOB_SETTINGS_GET`, caches the result and invokes the
 *    callback when the reply arrives;
 *  - @ref settings_invalidate drops an entry after a write so the next read
 *    refetches.
 *
 * Time comes from the injected clock (SPEC-ADDENDUM-TOOLCHAIN A3); the module
 * never calls the real clock.
 *
 * @ingroup cache
 */
#ifndef TG_SETTINGS_H
#define TG_SETTINGS_H

#include <stdint.h>

#include "tg/db.h"
#include "tg/util.h"

/** @brief Settings-cache time-to-live in milliseconds. */
#define SETTINGS_TTL_MS (5ULL * 60ULL * 1000ULL)

/** @brief Settings-lookup callback; runs on the loop thread. */
typedef void (*settings_cb)(const chat_settings_t *settings, int status, void *ctx);

/** @brief The settings service (opaque; see settings.c). */
typedef struct settings settings_t;

/** @brief Configuration for the settings service. All pointers must outlive it. */
typedef struct {
    const db_ops_t *db;       /**< DB seam (settings query). */
    const clock_ops_t *clock; /**< Injected clock for the cache TTL. */
} settings_config_t;

/**
 * @brief Create the settings service.
 *
 * @param[in] cfg Configuration (copied; pointers must outlive the service).
 *
 * @return A new service, or NULL on failure.
 * @ownership Caller must call settings_destroy().
 * @thread_safety Loop thread only.
 */
settings_t *settings_create(const settings_config_t *cfg);

/**
 * @brief Destroy the service and its cache.
 *
 * @param[in] s Service (may be NULL).
 * @thread_safety Loop thread only.
 */
void settings_destroy(settings_t *s);

/**
 * @brief Fetch (or reuse) the policy for @p chat_id.
 *
 * On a cache hit @p cb runs synchronously with the cached value; on a miss it
 * submits the DB query and invokes @p cb when the reply arrives. The pointer
 * passed to @p cb is valid only for the duration of the call (copy what must
 * outlive it).
 *
 * @param[in] s       Service.
 * @param[in] chat_id Chat to query.
 * @param[in] cb      Callback receiving the settings and status (never NULL).
 * @param[in] ctx     Opaque pointer passed to @p cb.
 *
 * @return 0 if the request was started or resolved synchronously, negative errno
 *         if it could not be started (the callback does not run then).
 * @ownership @p ctx must stay valid until @p cb runs.
 * @thread_safety Loop thread only.
 */
int settings_get(settings_t *s, int64_t chat_id, settings_cb cb, void *ctx);

/**
 * @brief Drop the cached policy for @p chat_id.
 *
 * Call after a successful settings write so the next read sees the new value.
 *
 * @param[in] s       Service.
 * @param[in] chat_id Chat whose cache is dropped.
 * @thread_safety Loop thread only.
 */
void settings_invalidate(settings_t *s, int64_t chat_id);

#endif /* TG_SETTINGS_H */
