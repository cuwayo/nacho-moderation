/**
 * @file captcha.c
 * @brief Join verification (captcha) flow (see captcha.h).
 *
 * @ingroup automod
 */
#include "tg/captcha.h"

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
#include "tg/settings.h"
#include "tg/tg_api.h"
#include "tg/tg_types.h"
#include "tg/util.h"

/** @brief Callback-data prefix for a captcha button. */
#define CAPTCHA_CB_PREFIX "cap:"

/** @brief All send permissions false (restrict): the challenged member can only read. */
static const char *const CAPTCHA_PERMS_FALSE =
    "\"can_send_messages\":false,\"can_send_audios\":false,\"can_send_documents\":false,"
    "\"can_send_photos\":false,\"can_send_videos\":false,\"can_send_video_notes\":false,"
    "\"can_send_voice_notes\":false,\"can_send_polls\":false,\"can_send_other_messages\":false,"
    "\"can_add_web_page_previews\":false,\"can_change_info\":false,\"can_invite_users\":false,"
    "\"can_pin_messages\":false";
/** @brief The common group permissions, granted when a member passes the challenge. */
static const char *const CAPTCHA_PERMS_TRUE =
    "\"can_send_messages\":true,\"can_send_audios\":true,\"can_send_documents\":true,"
    "\"can_send_photos\":true,\"can_send_videos\":true,\"can_send_video_notes\":true,"
    "\"can_send_voice_notes\":true,\"can_send_polls\":true,\"can_send_other_messages\":true,"
    "\"can_add_web_page_previews\":true,\"can_invite_users\":true";

/** @brief The captcha service. */
struct captcha {
    captcha_config_t cfg; /**< Copied configuration. */
    op_t timer;           /**< The periodic sweep timer op. */
    int armed;            /**< 1 while the timer SQE is outstanding. */
    int stopped;          /**< 1 once captcha_stop was called. */
};

/* --- Pure parser ----------------------------------------------------------- */

int captcha_parse_callback(const char *data, int64_t *out_chat, int64_t *out_user, char *nonce,
                           size_t nonce_cap)
{
    if (data == NULL || out_chat == NULL || out_user == NULL || nonce == NULL || nonce_cap == 0) {
        return -EINVAL;
    }
    if (strncmp(data, CAPTCHA_CB_PREFIX, strlen(CAPTCHA_CB_PREFIX)) != 0) {
        return -EINVAL;
    }
    const char *p = data + strlen(CAPTCHA_CB_PREFIX);
    char *end = NULL;
    long long chat = strtoll(p, &end, 10);
    if (end == p || *end != ':') {
        return -EINVAL;
    }
    p = end + 1;
    long long user = strtoll(p, &end, 10);
    if (end == p || *end != ':') {
        return -EINVAL;
    }
    p = end + 1;
    size_t nlen = strlen(p);
    if (nlen == 0 || nlen >= nonce_cap) {
        return -EINVAL;
    }
    *out_chat = chat;
    *out_user = user;
    (void)memcpy(nonce, p, nlen + 1u);
    return 0;
}

/* --- Nonce ----------------------------------------------------------------- */

/**
 * @brief Generate a fresh captcha nonce (hex) from the injectable random source.
 *
 * @param[in]  cap   Service.
 * @param[out] out   Destination.
 * @param[in]  cap_s Capacity of @p out (>= 2 * CAPTCHA_NONCE_LEN + 1).
 */
static void captcha_make_nonce(const captcha_t *cap, char *out, size_t cap_s)
{
    uint64_t r = util_random_u64(cap->cfg.random);
    (void)snprintf(out, cap_s, "%08llx", (unsigned long long)(r & 0xFFFFFFFFULL));
}

/* --- API helpers ----------------------------------------------------------- */

/**
 * @brief No-op completion for fire-and-forget calls.
 *
 * The API seam requires a non-NULL callback, so fire-and-forget calls pass this.
 *
 * @param[in] res API result (ignored).
 * @param[in] ctx Unused.
 */
static void on_ignore(const api_result_t *res, void *ctx)
{
    (void)res;
    (void)ctx;
}

/**
 * @brief Fire-and-forget API call helper.
 *
 * @param[in] cap    Service.
 * @param[in] method API method.
 * @param[in] body   JSON body.
 * @param[in] len    Body length.
 * @param[in] chat   Chat for per-chat rate limiting.
 * @param[in] cb     Completion callback (may be NULL for fire-and-forget).
 * @param[in] user   Callback context.
 *
 * @return 0 if queued, negative errno on failure.
 */
static int captcha_request(const captcha_t *cap, method_id_t method, const char *body, size_t len,
                           int64_t chat, tg_api_cb cb, void *user)
{
    if (cap->cfg.api == NULL || cap->cfg.api->request == NULL) {
        return -EINVAL;
    }
    /* The seam requires a non-NULL callback; substitute a no-op. */
    return cap->cfg.api->request(cap->cfg.api->ctx, method, body, len, chat, 0,
                                 cb != NULL ? cb : on_ignore, user);
}

/**
 * @brief Issue `restrictChatMember` granting or revoking the common rights.
 *
 * @param[in] cap    Service.
 * @param[in] chat   Chat.
 * @param[in] user   User.
 * @param[in] grant  1 to restore permissions, 0 to mute.
 * @param[in] cb     Completion callback (may be NULL).
 * @param[in] cbu    Callback context.
 *
 * @return 0 if queued, negative errno on failure.
 */
static int captcha_restrict(const captcha_t *cap, int64_t chat, int64_t user, int grant,
                            tg_api_cb cb, void *cbu)
{
    char body[1024];
    int n = snprintf(body, sizeof(body), "{\"chat_id\":%lld,\"user_id\":%lld,\"permissions\":{%s}}",
                     (long long)chat, (long long)user,
                     grant != 0 ? CAPTCHA_PERMS_TRUE : CAPTCHA_PERMS_FALSE);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return -ENOSPC;
    }
    return captcha_request(cap, METHOD_RESTRICT_CHAT_MEMBER, body, (size_t)n, chat, cb, cbu);
}

/**
 * @brief Delete one message (fire and forget).
 *
 * @param[in] cap    Service.
 * @param[in] chat   Chat.
 * @param[in] msg_id Message id (0 = skip).
 */
static void captcha_delete(const captcha_t *cap, int64_t chat, int64_t msg_id)
{
    if (msg_id == 0) {
        return;
    }
    char body[64];
    int n = snprintf(body, sizeof(body), "{\"chat_id\":%lld,\"message_id\":%lld}", (long long)chat,
                     (long long)msg_id);
    if (n > 0 && (size_t)n < sizeof(body)) {
        (void)captcha_request(cap, METHOD_DELETE_MESSAGE, body, (size_t)n, chat, NULL, NULL);
    }
}

/* --- Pending-row storage --------------------------------------------------- */

/** @brief Context for storing a pending captcha row after the prompt is sent. */
typedef struct {
    captcha_t *cap;                          /**< Service. */
    int64_t chat_id;                         /**< Chat. */
    int64_t user_id;                         /**< Challenged user. */
    char nonce[CAPTCHA_NONCE_LEN * 2u + 1u]; /**< Nonce. */
    int64_t deadline;                        /**< Unix seconds the challenge expires. */
} captcha_pending_ctx_t;

static void on_prompt_sent(const api_result_t *res, void *ctx);

/**
 * @brief Store the pending row once the prompt message id is known.
 *
 * @param[in] res API result.
 * @param[in] ctx The @ref captcha_pending_ctx_t (freed here).
 */
static void on_prompt_sent(const api_result_t *res, void *ctx)
{
    captcha_pending_ctx_t *p = ctx;
    int64_t message_id = 0;
    if (res->status == 0 && res->body != NULL) {
        const char *m = strstr(res->body, "\"message_id\":");
        if (m != NULL) {
            char *end = NULL;
            long long v = strtoll(m + 13, &end, 10);
            if (end != m + 13) {
                message_id = (int64_t)v;
            }
        }
    }
    if (p->cap->cfg.db != NULL && p->cap->cfg.db->submit != NULL) {
        db_job_t job = {.kind = DB_JOB_CAPTCHA_PUT,
                        .id = p->chat_id,
                        .user_id = p->user_id,
                        .message_id = message_id,
                        .expires_at = p->deadline};
        (void)snprintf(job.nonce, sizeof(job.nonce), "%s", p->nonce);
        (void)p->cap->cfg.db->submit(p->cap->cfg.db->ctx, &job, NULL, NULL);
    }
    free(p);
}

/**
 * @brief Send the "I'm human" prompt and record the pending row.
 *
 * @param[in] cap   Service.
 * @param[in] chat  Chat.
 * @param[in] user  User.
 * @param[in] nonce Nonce.
 * @param[in] deadline Unix expiry.
 */
static void captcha_send_prompt(const captcha_t *cap, int64_t chat, const tg_user_t *user,
                                const char *nonce, int64_t deadline)
{
    captcha_pending_ctx_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return;
    }
    p->cap = (captcha_t *)cap;
    p->chat_id = chat;
    p->user_id = user->id;
    p->deadline = deadline;
    (void)snprintf(p->nonce, sizeof(p->nonce), "%s", nonce);

    char cb[64];
    (void)snprintf(cb, sizeof(cb), CAPTCHA_CB_PREFIX "%lld:%lld:%s", (long long)chat,
                   (long long)user->id, nonce);
    char body[1024];
    int n = snprintf(body, sizeof(body),
                     "{\"chat_id\":%lld,\"text\":\"Welcome! Please confirm you are human to "
                     "stay in the group.\",\"reply_markup\":{\"inline_keyboard\":[[{\"text\":"
                     "\"I'm human\",\"callback_data\":\"%s\"}]]}}",
                     (long long)chat, cb);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        free(p);
        return;
    }
    if (captcha_request(cap, METHOD_SEND_MESSAGE, body, (size_t)n, chat, on_prompt_sent, p) != 0) {
        free(p);
    }
}

/* --- Join handling --------------------------------------------------------- */

/** @brief Context for one join's settings lookup. */
typedef struct {
    captcha_t *cap; /**< Service. */
    tg_chat_t chat; /**< Chat joined. */
    tg_user_t user; /**< Joining user. */
} captcha_join_ctx_t;

static void on_join_settings(const chat_settings_t *settings, int status, void *ctx);

/**
 * @brief Settings-fetch completion for a join: challenge or welcome.
 *
 * @param[in] settings Chat policy.
 * @param[in] status   Lookup status.
 * @param[in] ctx      The @ref captcha_join_ctx_t (freed here).
 */
static void on_join_settings(const chat_settings_t *settings, int status, void *ctx)
{
    captcha_join_ctx_t *j = ctx;
    int enabled = status == 0 && settings != NULL && settings->captcha_enabled != 0;
    if (!enabled) {
        if (j->cap->cfg.on_welcome != NULL) {
            j->cap->cfg.on_welcome(j->cap->cfg.on_welcome_ctx, &j->chat, &j->user);
        }
        free(j);
        return;
    }
    int timeout =
        (settings->captcha_timeout > 0) ? settings->captcha_timeout : CAPTCHA_DEFAULT_TIMEOUT;
    int64_t deadline = wall_now_unix(j->cap->cfg.wall) + timeout;
    char nonce[CAPTCHA_NONCE_LEN * 2u + 1u];
    captcha_make_nonce(j->cap, nonce, sizeof(nonce));
    /* Restrict first (no message to the user), then send the prompt. */
    (void)captcha_restrict(j->cap, j->chat.id, j->user.id, 0, NULL, NULL);
    captcha_send_prompt(j->cap, j->chat.id, &j->user, nonce, deadline);
    free(j);
}

int captcha_begin(captcha_t *cap, const tg_chat_t *chat, const tg_user_t *user)
{
    if (cap == NULL || chat == NULL || user == NULL) {
        return -EINVAL;
    }
    if (user->is_bot || user->id == 0) {
        return 0;
    }
    if (cap->cfg.settings == NULL) {
        /* No policy source: greet as usual. */
        if (cap->cfg.on_welcome != NULL) {
            cap->cfg.on_welcome(cap->cfg.on_welcome_ctx, chat, user);
        }
        return 0;
    }
    captcha_join_ctx_t *j = calloc(1, sizeof(*j));
    if (j == NULL) {
        return -ENOMEM;
    }
    j->cap = cap;
    j->chat = *chat;
    j->user = *user;
    if (settings_get((settings_t *)cap->cfg.settings, chat->id, on_join_settings, j) != 0) {
        free(j);
        return 0; /* the join is simply not greeted */
    }
    return 0;
}

/* --- Callback handling ----------------------------------------------------- */

/**
 * @brief Answer a callback query (stop the client spinner).
 *
 * @param[in] cap     Service.
 * @param[in] chat    Chat for rate limiting.
 * @param[in] cb_id   Callback query id.
 * @param[in] text    Toast text (may be NULL).
 */
static void captcha_answer(const captcha_t *cap, int64_t chat, const char *cb_id, const char *text)
{
    char body[256];
    int n;
    if (text != NULL) {
        n = snprintf(body, sizeof(body), "{\"callback_query_id\":\"%s\",\"text\":\"%s\"}", cb_id,
                     text);
    } else {
        n = snprintf(body, sizeof(body), "{\"callback_query_id\":\"%s\"}", cb_id);
    }
    if (n > 0 && (size_t)n < sizeof(body)) {
        (void)captcha_request(cap, METHOD_ANSWER_CALLBACK_QUERY, body, (size_t)n, chat, on_ignore,
                              NULL);
    }
}

/** @brief Context for one captcha callback verification. */
typedef struct {
    captcha_t *cap;                          /**< Service. */
    int64_t chat_id;                         /**< Chat. */
    int64_t user_id;                         /**< Challenged user. */
    int64_t message_id;                      /**< Prompt message id. */
    char nonce[CAPTCHA_NONCE_LEN * 2u + 1u]; /**< Nonce from the callback data. */
    char callback_id[64];                    /**< Callback query id (for answerCallbackQuery). */
} captcha_cb_ctx_t;

static void on_captcha_row(const db_result_t *res, void *ctx);

/**
 * @brief DB completion for the pending-row fetch: verify and lift on success.
 *
 * @param[in] res DB result.
 * @param[in] ctx The @ref captcha_cb_ctx_t (freed here).
 */
static void on_captcha_row(const db_result_t *res, void *ctx)
{
    captcha_cb_ctx_t *b = ctx;
    captcha_t *cap = b->cap;
    int ok =
        res->status == 0 && res->captcha_found != 0 && strcmp(res->captcha[0].nonce, b->nonce) == 0;
    if (!ok) {
        captcha_answer(cap, b->chat_id, b->callback_id, "This challenge has expired.");
        free(b);
        return;
    }
    /* Lift the restriction, delete the prompt, drop the row and welcome. */
    (void)captcha_restrict(cap, b->chat_id, b->user_id, 1, NULL, NULL);
    captcha_delete(cap, b->chat_id, b->message_id);
    if (cap->cfg.db != NULL && cap->cfg.db->submit != NULL) {
        db_job_t del = {.kind = DB_JOB_CAPTCHA_DEL, .id = b->chat_id, .user_id = b->user_id};
        (void)cap->cfg.db->submit(cap->cfg.db->ctx, &del, NULL, NULL);
    }
    if (cap->cfg.on_welcome != NULL) {
        tg_chat_t chat = {.id = b->chat_id};
        (void)snprintf(chat.type, sizeof(chat.type), "supergroup");
        tg_user_t user = {.id = b->user_id};
        cap->cfg.on_welcome(cap->cfg.on_welcome_ctx, &chat, &user);
    }
    captcha_answer(cap, b->chat_id, b->callback_id, "Verified. Welcome!");
    free(b);
}

int captcha_handle_callback(captcha_t *cap, const tg_update_t *upd)
{
    if (cap == NULL || upd == NULL) {
        return 0;
    }
    int64_t chat = 0;
    int64_t user = 0;
    char nonce[CAPTCHA_NONCE_LEN * 2u + 1u];
    if (captcha_parse_callback(upd->callback_data, &chat, &user, nonce, sizeof(nonce)) != 0) {
        return 0; /* not a captcha payload */
    }
    /* Ownership (SPEC section 10): only the challenged user may press. */
    if (upd->callback_from.id != user) {
        captcha_answer(cap, chat, upd->callback_id, "This challenge is not yours.");
        return 1;
    }
    captcha_cb_ctx_t *b = calloc(1, sizeof(*b));
    if (b == NULL) {
        return 1;
    }
    b->cap = cap;
    b->chat_id = chat;
    b->user_id = user;
    b->message_id = upd->callback_message_id;
    (void)snprintf(b->nonce, sizeof(b->nonce), "%s", nonce);
    (void)snprintf(b->callback_id, sizeof(b->callback_id), "%s", upd->callback_id);
    if (cap->cfg.db == NULL || cap->cfg.db->submit == NULL) {
        captcha_answer(cap, chat, upd->callback_id, "This challenge has expired.");
        free(b);
        return 1;
    }
    db_job_t job = {.kind = DB_JOB_CAPTCHA_GET, .id = chat, .user_id = user};
    if (cap->cfg.db->submit(cap->cfg.db->ctx, &job, on_captcha_row, b) != 0) {
        captcha_answer(cap, chat, upd->callback_id, "This challenge has expired.");
        free(b);
    }
    return 1;
}

/* --- Timeout sweep --------------------------------------------------------- */

/**
 * @brief Kick a member whose captcha expired: ban then unban (SPEC section 5.4).
 *
 * @param[in] cap  Service.
 * @param[in] chat Chat.
 * @param[in] user User.
 */
static void captcha_kick(const captcha_t *cap, int64_t chat, int64_t user)
{
    char body[128];
    int n = snprintf(body, sizeof(body), "{\"chat_id\":%lld,\"user_id\":%lld}", (long long)chat,
                     (long long)user);
    if (n <= 0 || (size_t)n >= sizeof(body)) {
        return;
    }
    (void)captcha_request(cap, METHOD_BAN_CHAT_MEMBER, body, (size_t)n, chat, on_ignore, NULL);
    /* Immediately unban so the kick is a removal, not a permanent ban. */
    char ub[160];
    int un = snprintf(ub, sizeof(ub), "{\"chat_id\":%lld,\"user_id\":%lld,\"only_if_banned\":true}",
                      (long long)chat, (long long)user);
    if (un > 0 && (size_t)un < sizeof(ub)) {
        (void)captcha_request(cap, METHOD_UNBAN_CHAT_MEMBER, ub, (size_t)un, chat, on_ignore, NULL);
    }
}

/**
 * @brief Expiry-query completion: kick and clean up every due challenge.
 *
 * @param[in] res DB result carrying the due rows.
 * @param[in] user The @ref captcha_t.
 */
// cppcheck-suppress constParameterCallback
static void on_captcha_expired(const db_result_t *res, void *user)
{
    const captcha_t *cap = user;
    if (res->status != 0) {
        util_log(LOG_WARN, "captcha", "expiry query failed: %d", res->status);
        return;
    }
    for (unsigned i = 0; i < res->captcha_count; ++i) {
        const db_captcha_t *c = &res->captcha[i];
        captcha_kick(cap, c->chat_id, c->user_id);
        captcha_delete(cap, c->chat_id, c->message_id);
        if (cap->cfg.db != NULL && cap->cfg.db->submit != NULL) {
            db_job_t del = {.kind = DB_JOB_CAPTCHA_DEL, .id = c->chat_id, .user_id = c->user_id};
            (void)cap->cfg.db->submit(cap->cfg.db->ctx, &del, NULL, NULL);
        }
    }
    if (res->captcha_count > 0) {
        util_log(LOG_INFO, "captcha", "kicked %u member(s) that failed the captcha",
                 res->captcha_count);
    }
}

int captcha_sweep_now(captcha_t *cap)
{
    if (cap == NULL || cap->cfg.db == NULL || cap->cfg.db->submit == NULL) {
        return -EINVAL;
    }
    db_job_t job = {.kind = DB_JOB_CAPTCHA_EXPIRE, .now_unix = wall_now_unix(cap->cfg.wall)};
    return cap->cfg.db->submit(cap->cfg.db->ctx, &job, on_captcha_expired, cap);
}

/* --- Timer ----------------------------------------------------------------- */

/**
 * @brief Timer completion: run a sweep and re-arm the periodic timer.
 *
 * @param[in] op  The timer op (its owner is the @ref captcha_t).
 * @param[in] res Timer result (ignored).
 */
static void on_captcha_timer(struct op *op, int res)
{
    (void)res;
    captcha_t *cap = op->owner;
    cap->armed = 0;
    if (cap->stopped) {
        return;
    }
    (void)captcha_sweep_now(cap);
    if (cap->cfg.ops->prep_timeout(cap->cfg.ops->ctx, &cap->timer, cap->cfg.interval_ms) == 0) {
        cap->armed = 1;
    }
}

/* --- Lifecycle ------------------------------------------------------------- */

captcha_t *captcha_create(const captcha_config_t *cfg)
{
    if (cfg == NULL || cfg->ops == NULL || cfg->api == NULL || cfg->wall == NULL ||
        cfg->random == NULL) {
        return NULL;
    }
    captcha_t *cap = calloc(1, sizeof(*cap));
    if (cap == NULL) {
        return NULL;
    }
    cap->cfg = *cfg;
    if (cap->cfg.interval_ms == 0) {
        cap->cfg.interval_ms = CAPTCHA_SWEEP_INTERVAL_MS;
    }
    cap->timer =
        (op_t){.type = OP_TIMEOUT, .owner = cap, .on_complete = on_captcha_timer, .fd = -1};
    return cap;
}

int captcha_start(captcha_t *cap)
{
    if (cap == NULL) {
        return -EINVAL;
    }
    cap->stopped = 0;
    int rc = cap->cfg.ops->prep_timeout(cap->cfg.ops->ctx, &cap->timer, cap->cfg.interval_ms);
    if (rc == 0) {
        cap->armed = 1;
    }
    return rc;
}

void captcha_stop(captcha_t *cap)
{
    if (cap != NULL) {
        cap->stopped = 1;
    }
}

void captcha_set_welcome_ctx(captcha_t *cap, void *ctx)
{
    if (cap != NULL) {
        cap->cfg.on_welcome_ctx = ctx;
    }
}

void captcha_destroy(captcha_t *cap)
{
    free(cap);
}
