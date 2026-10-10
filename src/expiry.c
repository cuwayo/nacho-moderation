/**
 * @file expiry.c
 * @brief Periodic lifter for expired temp bans and mutes (see expiry.h).
 *
 * @ingroup commands
 */
#include "tg/expiry.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_methods.h"
#include "tables.h"
#include "tg/api_pool.h"
#include "tg/db.h"
#include "tg/loop.h"
#include "tg/tg_api.h"
#include "tg/util.h"

/** @brief The sweeper state. */
struct expiry {
    expiry_config_t cfg; /**< Copied configuration. */
    op_t timer;          /**< The periodic timer op. */
    int armed;           /**< 1 while the timer SQE is outstanding. */
    int stopped;         /**< 1 once expiry_stop was called. */
};

/**
 * @brief Timer completion: run a sweep and re-arm the periodic timer.
 *
 * @param[in] op  The timer op (its owner is the @ref expiry_t).
 * @param[in] res Timer result (ignored).
 */
static void on_expiry_timer(struct op *op, int res)
{
    (void)res;
    expiry_t *e = op->owner;
    e->armed = 0;
    if (e->stopped) {
        return;
    }
    (void)expiry_sweep_now(e);
    /* Re-arm the interval timer. */
    if (e->cfg.ops->prep_timeout(e->cfg.ops->ctx, &e->timer, e->cfg.interval_ms) == 0) {
        e->armed = 1;
    }
}

/**
 * @brief Build the `restrictChatMember` body that restores the common group rights.
 *
 * @param[out] body     Destination buffer.
 * @param[in]  cap      Capacity of @p body.
 * @param[in]  chat_id  Chat.
 * @param[in]  user_id  User.
 * @param[out] outlen   Receives the body length.
 *
 * @return 0 on success, -ENOSPC if @p body is too small.
 */
static int build_unmute(char *body, size_t cap, int64_t chat_id, int64_t user_id, size_t *outlen)
{
    static const char *const perms_true =
        "\"can_send_messages\":true,\"can_send_audios\":true,\"can_send_documents\":true,"
        "\"can_send_photos\":true,\"can_send_videos\":true,\"can_send_video_notes\":true,"
        "\"can_send_voice_notes\":true,\"can_send_polls\":true,\"can_send_other_messages\":true,"
        "\"can_add_web_page_previews\":true,\"can_invite_users\":true";
    int n = snprintf(body, cap, "{\"chat_id\":%lld,\"user_id\":%lld,\"permissions\":{%s}}",
                     (long long)chat_id, (long long)user_id, perms_true);
    if (n < 0 || (size_t)n >= cap) {
        return -ENOSPC;
    }
    *outlen = (size_t)n;
    return 0;
}

/**
 * @brief Fire-and-forget completion for a lift call.
 *
 * @param[in] res API result.
 * @param[in] user Unused.
 */
static void on_lift(const api_result_t *res, void *user)
{
    (void)user;
    if (res->status != 0) {
        util_log(LOG_WARN, "expiry", "failed to lift an expired action: %d", res->status);
    }
}

/**
 * @brief DB completion for the lift-row update (fire and forget).
 *
 * @param[in] res DB result.
 * @param[in] user Unused.
 */
static void on_lift_db(const db_result_t *res, void *user);

/**
 * @brief Issue the API call that lifts one expired action and mark the row lifted.
 *
 * @param[in] e The sweeper (for the API seam).
 * @param[in] a The expired action.
 */
static void lift_one(const expiry_t *e, const db_expired_action_t *a)
{
    char body[512];
    size_t len = 0;
    if (strcmp(a->action, "ban") == 0) {
        int n = snprintf(body, sizeof(body),
                         "{\"chat_id\":%lld,\"user_id\":%lld,\"only_if_banned\":true}",
                         (long long)a->chat_id, (long long)a->user_id);
        if (n < 0 || (size_t)n >= sizeof(body)) {
            return;
        }
        len = (size_t)n;
        (void)e->cfg.api->request(e->cfg.api->ctx, METHOD_UNBAN_CHAT_MEMBER, body, len, a->chat_id,
                                  0, on_lift, NULL);
    } else if (strcmp(a->action, "mute") == 0) {
        if (build_unmute(body, sizeof(body), a->chat_id, a->user_id, &len) == 0) {
            (void)e->cfg.api->request(e->cfg.api->ctx, METHOD_RESTRICT_CHAT_MEMBER, body, len,
                                      a->chat_id, 0, on_lift, NULL);
        }
    }
    /* Any other action (e.g. "kick", which does not expire) only clears the row. */

    /* Mark the row lifted so it is never applied twice, even if the call failed
     * (a transient failure is logged; re-lifting would need a retry queue, M5). */
    db_job_t job = {.kind = DB_JOB_LIFT_ACTION, .id = a->id};
    (void)e->cfg.db->submit(e->cfg.db->ctx, &job, on_lift_db, NULL);
}

/**
 * @brief Expiry-query completion: lift every due action.
 *
 * @param[in] res DB result carrying the due rows.
 * @param[in] user The @ref expiry_t.
 */
// cppcheck-suppress constParameterCallback
static void on_expired(const db_result_t *res, void *user)
{
    const expiry_t *e = user;
    if (res->status != 0) {
        util_log(LOG_WARN, "expiry", "expiry query failed: %d", res->status);
        return;
    }
    for (unsigned i = 0; i < res->expired_count; ++i) {
        lift_one(e, &res->expired[i]);
    }
    if (res->expired_count > 0) {
        util_log(LOG_INFO, "expiry", "lifted %u expired action(s)", res->expired_count);
    }
}

int expiry_sweep_now(expiry_t *e)
{
    if (e == NULL || e->cfg.db == NULL || e->cfg.db->submit == NULL) {
        return -EINVAL;
    }
    db_job_t job = {.kind = DB_JOB_EXPIRE_ACTIONS, .now_unix = wall_now_unix(e->cfg.wall)};
    return e->cfg.db->submit(e->cfg.db->ctx, &job, on_expired, e);
}

expiry_t *expiry_create(const expiry_config_t *cfg)
{
    if (cfg == NULL || cfg->ops == NULL || cfg->db == NULL || cfg->api == NULL ||
        cfg->wall == NULL) {
        return NULL;
    }
    expiry_t *e = calloc(1, sizeof(*e));
    if (e == NULL) {
        return NULL;
    }
    e->cfg = *cfg;
    if (e->cfg.interval_ms == 0) {
        e->cfg.interval_ms = EXPIRY_INTERVAL_MS;
    }
    e->timer = (op_t){.type = OP_TIMEOUT, .owner = e, .on_complete = on_expiry_timer, .fd = -1};
    return e;
}

int expiry_start(expiry_t *e)
{
    if (e == NULL) {
        return -EINVAL;
    }
    e->stopped = 0;
    int rc = e->cfg.ops->prep_timeout(e->cfg.ops->ctx, &e->timer, e->cfg.interval_ms);
    if (rc == 0) {
        e->armed = 1;
    }
    return rc;
}

void expiry_stop(expiry_t *e)
{
    if (e != NULL) {
        e->stopped = 1;
    }
}

void expiry_destroy(expiry_t *e)
{
    free(e);
}

/**
 * @brief DB completion for the lift-row update (fire and forget).
 *
 * @param[in] res DB result.
 * @param[in] user Unused.
 */
static void on_lift_db(const db_result_t *res, void *user)
{
    (void)user;
    if (res->status != 0) {
        util_log(LOG_WARN, "expiry", "failed to mark an action lifted: %d", res->status);
    }
}
