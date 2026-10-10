/**
 * @file settings.c
 * @brief Per-chat moderation policy cache over the DB worker (see settings.h).
 *
 * @ingroup cache
 */
#include "tg/settings.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tables.h"
#include "tg/cache.h"
#include "tg/db.h"
#include "tg/util.h"

/** @brief A pending settings query awaiting its DB reply. */
typedef struct {
    settings_t *settings; /**< Service to cache into. */
    int64_t chat_id;      /**< Chat being fetched. */
    settings_cb cb;       /**< Caller callback. */
    void *ctx;            /**< Caller context. */
} settings_fetch_ctx_t;

/** @brief The settings service. */
struct settings {
    settings_config_t cfg; /**< Copied configuration. */
    cache_t *cache;        /**< chat_id -> chat_settings_t. */
};

/**
 * @brief Render the cache key for @p chat_id.
 *
 * @param[out] key Destination buffer (at least 32 bytes).
 * @param[in]  chat_id Chat id.
 */
static void settings_key(char *key, int64_t chat_id)
{
    (void)snprintf(key, 32, "%lld", (long long)chat_id);
}

/**
 * @brief DB completion: cache the policy and forward it to the caller.
 *
 * @param[in] res DB result carrying @c settings.
 * @param[in] ctx The @ref settings_fetch_ctx_t.
 */
static void on_settings_fetched(const db_result_t *res, void *ctx)
{
    settings_fetch_ctx_t *f = ctx;
    if (res->status == 0) {
        char key[32];
        settings_key(key, f->chat_id);
        if (cache_put(f->settings->cache, key, &res->settings, sizeof(res->settings),
                      SETTINGS_TTL_MS) != 0) {
            util_log(LOG_WARN, "settings", "failed to cache settings for %lld",
                     (long long)f->chat_id);
        }
        f->cb(&res->settings, 0, f->ctx);
    } else {
        f->cb(NULL, res->status, f->ctx);
    }
    free(f);
}

settings_t *settings_create(const settings_config_t *cfg)
{
    if (cfg == NULL || cfg->db == NULL || cfg->db->submit == NULL || cfg->clock == NULL) {
        return NULL;
    }
    settings_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->cfg = *cfg;
    s->cache = cache_create(cfg->clock, 256);
    if (s->cache == NULL) {
        free(s);
        return NULL;
    }
    return s;
}

void settings_destroy(settings_t *s)
{
    if (s == NULL) {
        return;
    }
    cache_destroy(s->cache);
    free(s);
}

int settings_get(settings_t *s, int64_t chat_id, settings_cb cb, void *ctx)
{
    if (s == NULL || cb == NULL) {
        return -EINVAL;
    }

    char key[32];
    settings_key(key, chat_id);
    size_t len = 0;
    const chat_settings_t *cached = cache_get(s->cache, key, &len);
    if (cached != NULL && len == sizeof(*cached)) {
        cb(cached, 0, ctx); /* synchronous hit */
        return 0;
    }

    settings_fetch_ctx_t *f = calloc(1, sizeof(*f));
    if (f == NULL) {
        return -ENOMEM;
    }
    f->settings = s;
    f->chat_id = chat_id;
    f->cb = cb;
    f->ctx = ctx;

    db_job_t job = {.kind = DB_JOB_SETTINGS_GET, .id = chat_id};
    int rc = s->cfg.db->submit(s->cfg.db->ctx, &job, on_settings_fetched, f);
    if (rc != 0) {
        free(f);
        return rc;
    }
    return 0;
}

void settings_invalidate(settings_t *s, int64_t chat_id)
{
    if (s == NULL) {
        return;
    }
    char key[32];
    settings_key(key, chat_id);
    (void)cache_remove(s->cache, key);
}
