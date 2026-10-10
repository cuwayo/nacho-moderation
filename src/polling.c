/**
 * @file polling.c
 * @brief getUpdates long-polling loop (SPEC section 4.1).
 *
 * One long-poll is in flight at a time on the dedicated polling connection. The
 * completion callback only records the result; the next poll is issued from
 * poller_arm(), which is called from the poller's own completion path (the pool
 * dispatches the callback from api_pool_pump, so re-arming here is safe).
 *
 * @ingroup polling
 */
#include "tg/polling.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "api_methods.h"
#include "metrics.h"
#include "tables.h"
#include "tg/api_pool.h"
#include "tg/errors.h"
#include "tg/tg_api.h"
#include "tg/util.h"

/** @brief Default deadline margin added to the long-poll timeout (milliseconds). */
#define POLL_DEFAULT_EXTRA_MS 10000u

/** @brief The getUpdates poller state. */
struct poller {
    poller_config_t cfg; /**< Copied configuration. */
    int running;         /**< 1 while polling. */
    int in_flight;       /**< 1 while a request is outstanding. */
    int64_t offset;      /**< Offset for the next getUpdates. */
};

/* --- Offset helper --------------------------------------------------------- */

int poller_advance_offset(int64_t *offset, const char *json, size_t len, unsigned *count)
{
    if (offset == NULL || json == NULL || count == NULL) {
        return -EINVAL;
    }
    *count = 0;

    yyjson_doc *doc = yyjson_read(json, len, 0);
    if (doc == NULL) {
        return -ERR_PARSE;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *ok = yyjson_obj_get(root, "ok");
    yyjson_val *result = yyjson_obj_get(root, "result");
    if (ok == NULL || !yyjson_get_bool(ok) || result == NULL || !yyjson_is_arr(result)) {
        yyjson_doc_free(doc);
        return -ERR_PARSE;
    }

    int64_t max_id = *offset - 1; /* so an empty batch leaves the offset unchanged */
    size_t idx = 0;
    size_t max = 0;
    yyjson_val *item = NULL;
    yyjson_arr_foreach(result, idx, max, item)
    {
        yyjson_val *uid = yyjson_obj_get(item, "update_id");
        if (uid != NULL && yyjson_is_int(uid)) {
            int64_t id = (int64_t)yyjson_get_sint(uid);
            if (id > max_id) {
                max_id = id;
            }
            (*count)++;
        }
    }
    if (*count > 0) {
        *offset = max_id + 1;
    }
    yyjson_doc_free(doc);
    return 0;
}

/* --- Polling loop ---------------------------------------------------------- */

/** @brief Forward declaration of the getUpdates completion callback. */
static void poll_on_result(const api_result_t *res, void *ctx);

/**
 * @brief Issue the next getUpdates if the poller is running and idle.
 *
 * The long-poll deadline is derived from the configured timeout (plus a margin),
 * not the pool's per-request timeout, so a long poll is not aborted early.
 *
 * @param[in] p Poller.
 */
static void poller_arm(poller_t *p)
{
    if (!p->running || p->in_flight) {
        return;
    }
    /* The long-poll deadline must exceed the server-side timeout, otherwise every
     * poll would abort; the pool's per-request timeout is not used for this. */
    uint64_t extra = p->cfg.extra_ms != 0 ? p->cfg.extra_ms : POLL_DEFAULT_EXTRA_MS;
    uint64_t now = clock_now_ms(p->cfg.clock);
    uint64_t dl = now + ((uint64_t)p->cfg.timeout_s * 1000u) + extra;

    /* Build the getUpdates body here: the poller owns the offset and timeout. */
    char body[256];
    size_t body_len = 0;
    int rc =
        tg_api_build_get_updates_body(body, sizeof(body), p->offset, p->cfg.timeout_s, &body_len);
    if (rc != 0) {
        util_log(LOG_ERROR, "poll", "failed to build getUpdates body: %d", rc);
        return;
    }

    /* Mark in-flight before issuing so a synchronous completion cannot re-enter
     * poller_arm and double-issue. */
    p->in_flight = 1;
    rc = p->cfg.ops->request(p->cfg.ops->ctx, METHOD_GET_UPDATES, body, body_len, 0, dl,
                             poll_on_result, p);
    if (rc != 0) {
        p->in_flight = 0;
        util_log(LOG_WARN, "poll", "failed to queue getUpdates: %d", rc);
    }
}

/**
 * @brief getUpdates completion: advance the offset on success and re-arm.
 *
 * The offset is never advanced on failure, so no update is lost; the next poll is
 * issued regardless of the outcome.
 *
 * @param[in] res Result from the API client.
 * @param[in] ctx The poller.
 */
static void poll_on_result(const api_result_t *res, void *ctx)
{
    poller_t *p = ctx;
    p->in_flight = 0;
    if (!p->running) {
        return;
    }

    if (res->status == 0) {
        unsigned count = 0;
        if (poller_advance_offset(&p->offset, res->body, res->body_len, &count) == 0) {
            metrics_add(METRIC_UPDATES_RECEIVED, count);
            if (count != 0) {
                util_log(LOG_DEBUG, "poll", "received %u update(s), offset=%lld", count,
                         (long long)p->offset);
            }
            poll_batch_t batch = {.count = count, .offset = p->offset};
            if (p->cfg.on_batch != NULL) {
                p->cfg.on_batch(&batch, p->cfg.ctx);
            }
        }
    } else {
        /* Do not advance the offset on failure: the batch will be re-fetched. */
        util_log(LOG_WARN, "poll", "getUpdates failed: %d (retrying)", res->status);
    }

    /* Re-arm regardless of outcome (the pool already applied retry/backoff for
     * the request itself; here we simply start the next long-poll). */
    poller_arm(p);
}

/* --- Public API ------------------------------------------------------------ */

poller_t *poller_create(const poller_config_t *cfg)
{
    if (cfg == NULL || cfg->ops == NULL || cfg->ops->request == NULL) {
        return NULL;
    }
    poller_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->cfg = *cfg;
    if (p->cfg.timeout_s == 0) {
        p->cfg.timeout_s = 30;
    }
    return p;
}

int poller_start(poller_t *poller)
{
    if (poller == NULL) {
        return -EINVAL;
    }
    poller->running = 1;
    poller_arm(poller);
    return poller->in_flight ? 0 : -EIO;
}

void poller_stop(poller_t *poller)
{
    if (poller != NULL) {
        poller->running = 0;
    }
}

int64_t poller_offset(const poller_t *poller)
{
    return poller == NULL ? 0 : poller->offset;
}

void poller_destroy(poller_t *poller)
{
    free(poller);
}
