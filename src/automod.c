/**
 * @file automod.c
 * @brief Automated moderation: locks, blacklist and antiflood (see automod.h).
 *
 * @ingroup automod
 */
#include "tg/automod.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief PCRE2 must be configured for 8-bit code units before including its header. */
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "api_methods.h"
#include "commands.h"
#include "tables.h"
#include "tg/api_pool.h"
#include "tg/cache.h"
#include "tg/cmd_ctx.h"
#include "tg/db.h"
#include "tg/perms.h"
#include "tg/settings.h"
#include "tg/tg_api.h"
#include "tg/tg_types.h"
#include "tg/util.h"

/** @brief Default antiflood mute duration in seconds when the config is 0. */
#define AUTOMOD_DEFAULT_MUTE_S 300u
/** @brief Cache TTL for a flood window (ms); bounds memory across idle chats. */
#define AUTOMOD_FLOOD_TTL_MS (10ULL * 60ULL * 1000ULL)
/** @brief Cache TTL for a chat's blacklist patterns (ms). */
#define AUTOMOD_BLACKLIST_TTL_MS (5ULL * 60ULL * 1000ULL)
/** @brief PCRE2 match limit: caps backtracking work for one regex match (ReDoS guard). */
#define AUTOMOD_REGEX_MATCH_LIMIT 100000u
/** @brief PCRE2 depth limit: caps recursion depth for one regex match (ReDoS guard). */
#define AUTOMOD_REGEX_DEPTH_LIMIT 200u

/** @brief The automod service. */
struct automod {
    automod_config_t cfg; /**< Copied configuration. */
    cache_t *flood;       /**< `chat:user` -> @ref flood_window_t. */
    cache_t *blacklist;   /**< `chat` -> array of @ref db_blacklist_t. */
};

/* --- Pure classifiers ------------------------------------------------------ */

/**
 * @brief Report whether @p hay contains @p needle, ASCII case-insensitively.
 *
 * A portable replacement for `strcasestr` (a GNU extension the project does not
 * use). Only ASCII case folding is applied; UTF-8 bytes pass through unchanged.
 *
 * @param[in] hay    Haystack.
 * @param[in] needle Needle (empty matches).
 *
 * @return 1 on a match, 0 otherwise.
 */
static int contains_ci(const char *hay, const char *needle)
{
    if (hay == NULL || needle == NULL) {
        return 0;
    }
    size_t nl = strlen(needle);
    if (nl == 0) {
        return 1;
    }
    for (const char *p = hay; *p != '\0'; ++p) {
        size_t i = 0;
        while (i < nl && p[i] != '\0' &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nl) {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Return the text a message should be matched against (caption or text).
 *
 * @param[in] msg Message.
 * @return The non-empty text/caption, or "".
 */
static const char *message_body(const tg_message_t *msg)
{
    if (msg->text[0] != '\0') {
        return msg->text;
    }
    return msg->caption;
}

/**
 * @brief Return the lock bits implied by a message's entities and body text.
 *
 * @param[in] locks Chat lock mask (only bits present here are returned).
 * @param[in] msg   Message.
 * @return The matching entity/text-based lock bits.
 */
static unsigned entity_locks(unsigned locks, const tg_message_t *msg)
{
    unsigned hit = 0;
    for (unsigned i = 0; i < msg->entity_count; ++i) {
        const char *t = msg->entities[i].type;
        if (strcmp(t, "url") == 0 || strcmp(t, "text_link") == 0 || strcmp(t, "mention") == 0) {
            hit |= (unsigned)LOCK_LINKS;
        } else if (strcmp(t, "bot_command") == 0) {
            hit |= (unsigned)LOCK_COMMANDS;
        }
    }
    /* An invite link is a URL whose text is t.me/joinchat or t.me/+; entities
     * carry no text, so scan the body for the common forms. */
    const char *body = message_body(msg);
    if (contains_ci(body, "t.me/joinchat") || contains_ci(body, "t.me/+") ||
        contains_ci(body, "telegram.me/joinchat")) {
        hit |= (unsigned)LOCK_INVITES;
    }
    return hit & locks;
}

unsigned automod_locked_content(unsigned locks, const tg_message_t *msg)
{
    if (msg == NULL || locks == 0) {
        return 0;
    }
    unsigned hit = 0;
    if ((locks & (unsigned)LOCK_MEDIA) != 0 && (msg->content_flags & TG_CONTENT_MEDIA) != 0) {
        hit |= (unsigned)LOCK_MEDIA;
    }
    if ((locks & (unsigned)LOCK_STICKERS) != 0 &&
        (msg->content_flags & (TG_CONTENT_STICKER | TG_CONTENT_ANIMATION)) != 0) {
        hit |= (unsigned)LOCK_STICKERS;
    }
    if ((locks & (unsigned)LOCK_FORWARDS) != 0 && (msg->content_flags & TG_CONTENT_FORWARD) != 0) {
        hit |= (unsigned)LOCK_FORWARDS;
    }
    if ((locks & (unsigned)LOCK_POLLS) != 0 && (msg->content_flags & TG_CONTENT_POLL) != 0) {
        hit |= (unsigned)LOCK_POLLS;
    }
    if ((locks & (unsigned)LOCK_BOTS) != 0 && msg->from.is_bot) {
        hit |= (unsigned)LOCK_BOTS;
    }
    hit |= entity_locks(locks, msg);
    return hit;
}

int automod_blacklist_match(const char *pattern, int is_regex, const char *text)
{
    if (pattern == NULL || pattern[0] == '\0' || text == NULL || text[0] == '\0') {
        return 0;
    }
    if (is_regex == 0) {
        return contains_ci(text, pattern);
    }

    int errcode = 0;
    PCRE2_SIZE erroffset = 0;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED, PCRE2_CASELESS,
                                   &errcode, &erroffset, NULL);
    if (re == NULL) {
        return 0; /* an invalid pattern never matches */
    }
    pcre2_match_context *mc = pcre2_match_context_create(NULL);
    if (mc != NULL) {
        (void)pcre2_set_match_limit(mc, AUTOMOD_REGEX_MATCH_LIMIT);
        (void)pcre2_set_depth_limit(mc, AUTOMOD_REGEX_DEPTH_LIMIT);
    }
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    int matched = 0;
    if (md != NULL) {
        int rc = pcre2_match(re, (PCRE2_SPTR)text, strlen(text), 0, 0, md, mc);
        matched = rc >= 0 ? 1 : 0; /* NOMATCH (-1) and the limits (-47/-48) => 0 */
    }
    pcre2_match_data_free(md);
    pcre2_match_context_free(mc);
    pcre2_code_free(re);
    return matched;
}

unsigned automod_flood_track(flood_window_t *w, uint64_t now_ms, int64_t msg_id, uint64_t window_ms)
{
    if (w == NULL) {
        return 0;
    }
    uint64_t cutoff = now_ms > window_ms ? now_ms - window_ms : 0;
    /* Drop entries that have left the window (the array is oldest-first). */
    unsigned keep = 0;
    for (unsigned i = 0; i < w->count; ++i) {
        if (w->ts[i] >= cutoff) {
            w->ts[keep] = w->ts[i];
            w->msg_id[keep] = w->msg_id[i];
            keep++;
        }
    }
    w->count = keep;
    /* Make room if the ring is full, dropping the oldest tracked message. */
    if (w->count >= AUTOMOD_FLOOD_SLOTS) {
        memmove(&w->ts[0], &w->ts[1], (AUTOMOD_FLOOD_SLOTS - 1u) * sizeof(w->ts[0]));
        memmove(&w->msg_id[0], &w->msg_id[1], (AUTOMOD_FLOOD_SLOTS - 1u) * sizeof(w->msg_id[0]));
        w->count = AUTOMOD_FLOOD_SLOTS - 1u;
    }
    w->ts[w->count] = now_ms;
    w->msg_id[w->count] = msg_id;
    w->count++;
    return w->count;
}

/* --- Action helpers -------------------------------------------------------- */

/**
 * @brief Render a human label for a message's sender into @p out.
 *
 * @param[in]  user Sender.
 * @param[out] out  Destination.
 * @param[in]  cap  Capacity.
 */
static void sender_label(const tg_user_t *user, char *out, size_t cap)
{
    if (user->first_name[0] != '\0') {
        (void)snprintf(out, cap, "%s", user->first_name);
    } else if (user->username[0] != '\0') {
        (void)snprintf(out, cap, "@%s", user->username);
    } else {
        (void)snprintf(out, cap, "%lld", (long long)user->id);
    }
}

/**
 * @brief Build the `restrictChatMember` body that mutes a user until @p until.
 *
 * @param[out] body    Destination.
 * @param[in]  cap     Capacity.
 * @param[in]  chat_id Chat.
 * @param[in]  user_id User.
 * @param[in]  until   Unix seconds the mute expires (0 = permanent).
 * @param[out] outlen  Receives the length.
 *
 * @return 0 on success, -ENOSPC if @p body is too small.
 */
static int build_mute(char *body, size_t cap, int64_t chat_id, int64_t user_id, int64_t until,
                      size_t *outlen)
{
    static const char *const perms_false =
        "\"can_send_messages\":false,\"can_send_audios\":false,\"can_send_documents\":false,"
        "\"can_send_photos\":false,\"can_send_videos\":false,\"can_send_video_notes\":false,"
        "\"can_send_voice_notes\":false,\"can_send_polls\":false,\"can_send_other_messages\":false,"
        "\"can_add_web_page_previews\":false,\"can_change_info\":false,\"can_invite_users\":false,"
        "\"can_pin_messages\":false";
    int n;
    if (until > 0) {
        n = snprintf(body, cap,
                     "{\"chat_id\":%lld,\"user_id\":%lld,\"until_date\":%lld,\"permissions\":{%s}}",
                     (long long)chat_id, (long long)user_id, (long long)until, perms_false);
    } else {
        n = snprintf(body, cap, "{\"chat_id\":%lld,\"user_id\":%lld,\"permissions\":{%s}}",
                     (long long)chat_id, (long long)user_id, perms_false);
    }
    if (n < 0 || (size_t)n >= cap) {
        return -ENOSPC;
    }
    *outlen = (size_t)n;
    return 0;
}

/* --- Per-message evaluation ------------------------------------------------ */

/** @brief Everything resolved for one message's automod evaluation. */
typedef struct {
    automod_t *am;                             /**< Owning service. */
    tg_message_t msg;                          /**< Copied message (outlives async fetches). */
    chat_settings_t settings;                  /**< Chat policy snapshot. */
    int has_settings;                          /**< 1 once @ref settings is filled. */
    int is_exempt;                             /**< 1 if the sender is an admin/owner/sudo. */
    unsigned pattern_count;                    /**< Entries in @ref patterns. */
    db_blacklist_t patterns[DB_MAX_BLACKLIST]; /**< Copied blacklist patterns. */
} am_ctx_t;

/**
 * @brief Create a command invocation synthesizing the message for an action.
 *
 * The bot is the actor (so `actions.admin_id` is the bot) and the sender is the
 * target. Reuses the reply/record/log machinery so automod actions are logged
 * exactly like manual ones (SPEC section 5.5).
 *
 * @param[in] c The evaluation context.
 *
 * @return A new invocation (one reference held), or NULL on failure.
 */
static cmd_call_t *am_make_call(const am_ctx_t *c)
{
    cmd_call_t *call = cmd_call_create(c->am->cfg.api, c->am->cfg.db, c->am->cfg.wall);
    if (call == NULL) {
        return NULL;
    }
    struct cmd_ctx *ctx = cmd_call_ctx(call);
    tg_message_t *m = cmd_call_message(call);
    *m = c->msg;
    ctx->chat_id = c->msg.chat.id;
    ctx->user_id = c->am->cfg.bot_id; /* actor: the bot itself */
    ctx->bot_id = c->am->cfg.bot_id;
    ctx->target_id = c->msg.from.id;
    ctx->target_role = ROLE_ANYONE;
    ctx->args = "";
    ctx->is_group = 1;
    ctx->wall = c->am->cfg.wall;
    (void)cmd_ctx_set_settings(ctx, c->has_settings ? &c->settings : NULL);
    char label[CMD_TARGET_LABEL_MAX];
    sender_label(&c->msg.from, label, sizeof(label));
    (void)cmd_call_set_target_label(call, label);
    return call;
}

/**
 * @brief No-op completion for fire-and-forget API calls.
 *
 * The API seam requires a non-NULL callback, so fire-and-forget calls pass this.
 *
 * @param[in] res API result (ignored).
 * @param[in] ctx Unused.
 */
static void on_ignore_api(const api_result_t *res, void *ctx)
{
    (void)res;
    (void)ctx;
}

/**
 * @brief Delete one message through the API seam (fire and forget).
 *
 * @param[in] c      Evaluation context.
 * @param[in] msg_id Message to delete.
 */
static void am_delete(const am_ctx_t *c, int64_t msg_id)
{
    if (msg_id == 0 || c->am->cfg.api == NULL || c->am->cfg.api->request == NULL) {
        return;
    }
    char body[64];
    int n = snprintf(body, sizeof(body), "{\"chat_id\":%lld,\"message_id\":%lld}",
                     (long long)c->msg.chat.id, (long long)msg_id);
    if (n <= 0 || (size_t)n >= sizeof(body)) {
        return;
    }
    (void)c->am->cfg.api->request(c->am->cfg.api->ctx, METHOD_DELETE_MESSAGE, body, (size_t)n,
                                  c->msg.chat.id, 0, on_ignore_api, NULL);
}

/**
 * @brief Record a moderation action row and post it to the log channel.
 *
 * @param[in] call       Invocation (holds the seams and settings snapshot).
 * @param[in] action     Action name ("lock", "blacklist", "flood").
 * @param[in] target_id  Target user.
 * @param[in] reason     Reason text.
 * @param[in] expires_at Unix expiry (0 = permanent).
 */
static void am_record(cmd_call_t *call, const char *action, int64_t target_id, const char *reason,
                      int64_t expires_at)
{
    struct cmd_ctx *ctx = cmd_call_ctx(call);
    if (ctx != NULL && ctx->record_action != NULL) {
        ctx->record_action(ctx, action, target_id, reason, expires_at);
    }
}

/**
 * @brief Apply the configured warn action (ban/mute/kick) for a blacklist hit.
 *
 * Mirrors `/warn`'s auto-action: the public moderation handlers are reused so
 * the target guard, API call and `actions` row match a manual run.
 *
 * @param[in] ctx    Synthesized context (target resolved).
 * @param[in] action Action name.
 */
static void am_warn_action(struct cmd_ctx *ctx, const char *action)
{
    if (strcmp(action, "ban") == 0) {
        (void)cmd_ban(ctx);
    } else if (strcmp(action, "kick") == 0) {
        (void)cmd_kick(ctx);
    } else {
        (void)cmd_mute(ctx); /* default */
    }
}

/** @brief Context for the blacklist warn -> auto-action chain. */
typedef struct {
    cmd_call_t *call;                /**< Invocation kept alive across the DB job. */
    char action[DB_WARN_ACTION_MAX]; /**< Configured warn action. */
    int limit;                       /**< Warn limit at the time of the hit. */
} am_warn_ctx_t;

/**
 * @brief Warn-add completion: apply the configured action at the limit.
 *
 * @param[in] res DB result (its @c i64 is the new warn count).
 * @param[in] user The @ref am_warn_ctx_t.
 */
static void on_am_warn_added(const db_result_t *res, void *user)
{
    am_warn_ctx_t *w = user;
    if (res->status == 0 && w->limit > 0 && (int)res->i64 >= w->limit) {
        struct cmd_ctx *ctx = cmd_call_ctx(w->call);
        ctx->args = "";
        am_warn_action(ctx, w->action);
    }
    cmd_call_release(w->call);
    free(w);
}

/**
 * @brief Add a warning for a blacklist hit and, at the limit, apply the action.
 *
 * @param[in] c      Evaluation context.
 * @param[in] call   Invocation (already holds the settings snapshot).
 * @param[in] reason Reason text (the matched pattern).
 */
static void am_blacklist_warn(const am_ctx_t *c, cmd_call_t *call, const char *reason)
{
    struct cmd_ctx *ctx = cmd_call_ctx(call);
    if (ctx == NULL || ctx->db_submit == NULL) {
        return;
    }
    am_warn_ctx_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return;
    }
    w->call = call;
    cmd_call_retain(call);
    const char *action = (c->settings.warn_action[0] != '\0') ? c->settings.warn_action : "mute";
    (void)snprintf(w->action, sizeof(w->action), "%s", action);
    w->limit = c->settings.warn_limit;
    db_job_t job = {.kind = DB_JOB_WARN_ADD,
                    .id = c->msg.chat.id,
                    .user_id = c->msg.from.id,
                    .admin_id = c->am->cfg.bot_id,
                    .created_at = wall_now_unix(c->am->cfg.wall)};
    (void)snprintf(job.reason, sizeof(job.reason), "%s", reason != NULL ? reason : "");
    if (ctx->db_submit(ctx, &job, on_am_warn_added, w) != 0) {
        cmd_call_release(call);
        free(w);
    }
}

/**
 * @brief Return the name of the first lock bit set in @p hit (pure).
 *
 * @param[in] hit Lock bits (a subset of the `chats.locks` mask).
 *
 * @return The matching lock name, or "content" if none matched.
 */
static const char *lock_name(unsigned hit)
{
    /*! @cond */ /* X-macro expansion over locks.def (see SSOT S4). */
#define L(id, lname, bit, brief)                                                                   \
    if ((hit & (unsigned)LOCK_##id) != 0) {                                                        \
        return lname;                                                                              \
    }
#include "locks.def"
#undef L
    /*! @endcond */
    return "content";
}

/**
 * @brief Run the locks step for @p c (SPEC section 5.3 step 3).
 *
 * @param[in] c The evaluation context.
 *
 * @return 1 if a lock matched and the message was handled, else 0.
 */
static int am_apply_locks(const am_ctx_t *c)
{
    unsigned hit = automod_locked_content(c->settings.locks, &c->msg);
    if (hit == 0) {
        return 0;
    }
    cmd_call_t *call = am_make_call(c);
    if (call == NULL) {
        return 0;
    }
    am_delete(c, c->msg.message_id);
    am_record(call, "lock", c->msg.from.id, lock_name(hit), 0);
    cmd_call_release(call);
    return 1;
}

/**
 * @brief Run the blacklist step for @p c (SPEC section 5.3 step 4).
 *
 * @param[in] c The evaluation context.
 *
 * @return 1 if a pattern matched and the message was handled, else 0.
 */
static int am_apply_blacklist(const am_ctx_t *c)
{
    const char *body = message_body(&c->msg);
    if (body[0] == '\0' || c->pattern_count == 0) {
        return 0;
    }
    for (unsigned i = 0; i < c->pattern_count; ++i) {
        const db_blacklist_t *b = &c->patterns[i];
        if (automod_blacklist_match(b->pattern, b->is_regex, body) == 0) {
            continue;
        }
        cmd_call_t *call = am_make_call(c);
        if (call == NULL) {
            return 0;
        }
        am_delete(c, c->msg.message_id);
        am_record(call, "blacklist", c->msg.from.id, b->pattern, 0);
        am_blacklist_warn(c, call, b->pattern);
        cmd_call_release(call);
        return 1;
    }
    return 0;
}

/**
 * @brief Render the cache key for a (chat, user) flood window.
 *
 * @param[out] key  Destination (>= 48 bytes).
 * @param[in]  cap  Capacity of @p key.
 * @param[in]  chat Chat id.
 * @param[in]  user User id.
 */
static void flood_key(char *key, size_t cap, int64_t chat, int64_t user)
{
    (void)snprintf(key, cap, "%lld:%lld", (long long)chat, (long long)user);
}

/**
 * @brief Run the antiflood step for @p c (SPEC section 5.3 step 5).
 *
 * Tracks the message in the user's sliding window; when the window exceeds the
 * chat's limit, mutes the user and deletes the burst messages.
 *
 * @param[in] c The evaluation context.
 *
 * @return 1 if the user was muted, else 0.
 */
static int am_apply_flood(const am_ctx_t *c)
{
    if (c->settings.flood_limit <= 0 || c->am->flood == NULL) {
        return 0;
    }
    uint64_t window_s = c->settings.flood_window > 0 ? (uint64_t)c->settings.flood_window : 10u;
    uint64_t window_ms = window_s * 1000u;
    char key[48];
    flood_key(key, sizeof(key), c->msg.chat.id, c->msg.from.id);

    flood_window_t w = {0};
    size_t len = 0;
    const void *cached = cache_get(c->am->flood, key, &len);
    if (cached != NULL && len == sizeof(w)) {
        memcpy(&w, cached, sizeof(w));
    }
    uint64_t now = clock_now_ms(c->am->cfg.clock);
    unsigned count = automod_flood_track(&w, now, c->msg.message_id, window_ms);
    (void)cache_put(c->am->flood, key, &w, sizeof(w), AUTOMOD_FLOOD_TTL_MS);
    if ((int)count <= c->settings.flood_limit) {
        return 0;
    }

    cmd_call_t *call = am_make_call(c);
    if (call == NULL) {
        return 0;
    }
    unsigned mute_s =
        c->am->cfg.flood_mute_seconds > 0 ? c->am->cfg.flood_mute_seconds : AUTOMOD_DEFAULT_MUTE_S;
    int64_t until = wall_now_unix(c->am->cfg.wall) + (int64_t)mute_s;
    char body[1024];
    size_t blen = 0;
    if (build_mute(body, sizeof(body), c->msg.chat.id, c->msg.from.id, until, &blen) == 0) {
        (void)cmd_ctx_request(cmd_call_ctx(call), METHOD_RESTRICT_CHAT_MEMBER, body, blen, NULL,
                              NULL);
    }
    am_record(call, "flood", c->msg.from.id, "antiflood", until);
    /* Delete the burst messages that are still in the window. */
    for (unsigned i = 0; i < w.count; ++i) {
        am_delete(c, w.msg_id[i]);
    }
    cmd_call_release(call);
    return 1;
}

/**
 * @brief Evaluate a message once the settings, blacklist and admin list are known.
 *
 * @param[in] c The evaluation context (freed here).
 */
static void am_evaluate(am_ctx_t *c)
{
    /* Bots are ignored unless the chat locks them (locks.def `bots`). */
    if (c->msg.from.is_bot && (c->settings.locks & (unsigned)LOCK_BOTS) == 0) {
        free(c);
        return;
    }
    if (!c->is_exempt && c->has_settings) {
        /* SPEC section 5.3 order: locks -> blacklist -> antiflood. Locks and
         * blacklist are terminal (the message is removed); antiflood counts the
         * message and mutes a burst. */
        if (am_apply_locks(c) != 0) {
            free(c);
            return;
        }
        if (am_apply_blacklist(c) != 0) {
            free(c);
            return;
        }
        (void)am_apply_flood(c);
    }
    free(c);
}

/**
 * @brief Admin-fetch completion: derive the sender's role and evaluate.
 *
 * @param[in] admins Administrator list.
 * @param[in] status Lookup status.
 * @param[in] user   The @ref am_ctx_t.
 */
static void on_am_admins(const perms_admins_t *admins, int status, void *user)
{
    am_ctx_t *c = user;
    if (status == 0 && c->am->cfg.perms != NULL) {
        perms_subject_t subj = {0};
        if (perms_subject_from_admins(c->am->cfg.perms, admins, c->msg.from.id, &subj) == 0) {
            c->is_exempt = subj.role == ROLE_OWNER || subj.role == ROLE_ADMIN || subj.is_sudo;
        }
    }
    am_evaluate(c);
}

/**
 * @brief Fetch the administrator list, then evaluate.
 *
 * @param[in] c The evaluation context.
 */
static void am_fetch_admins(am_ctx_t *c)
{
    if (c->am->cfg.perms == NULL ||
        perms_admins_get(c->am->cfg.perms, c->msg.chat.id, on_am_admins, c) != 0) {
        am_evaluate(c);
    }
}

/**
 * @brief Blacklist-fetch completion: copy the patterns and fetch admins.
 *
 * The patterns are copied into the context so they stay valid after this
 * callback returns (the DB result is freed by the worker once the callback
 * completes), and also cached for the next message.
 *
 * @param[in] res  DB result carrying @c blacklist / @c blacklist_count.
 * @param[in] user The @ref am_ctx_t.
 */
static void on_am_blacklist(const db_result_t *res, void *user)
{
    am_ctx_t *c = user;
    if (res->status == 0 && res->blacklist_count > 0) {
        unsigned n = res->blacklist_count;
        if (n > DB_MAX_BLACKLIST) {
            n = DB_MAX_BLACKLIST;
        }
        memcpy(c->patterns, res->blacklist, n * sizeof(db_blacklist_t));
        c->pattern_count = n;
        char key[32];
        (void)snprintf(key, sizeof(key), "%lld", (long long)c->msg.chat.id);
        (void)cache_put(c->am->blacklist, key, c->patterns, n * sizeof(db_blacklist_t),
                        AUTOMOD_BLACKLIST_TTL_MS);
    }
    am_fetch_admins(c);
}

/**
 * @brief Load the chat's blacklist patterns (cache hit or DB fetch), then admins.
 *
 * @param[in] c The evaluation context.
 */
static void am_load_blacklist(am_ctx_t *c)
{
    char key[32];
    (void)snprintf(key, sizeof(key), "%lld", (long long)c->msg.chat.id);
    size_t len = 0;
    const void *cached = cache_get(c->am->blacklist, key, &len);
    if (cached != NULL && len % sizeof(db_blacklist_t) == 0) {
        unsigned n = (unsigned)(len / sizeof(db_blacklist_t));
        if (n > DB_MAX_BLACKLIST) {
            n = DB_MAX_BLACKLIST;
        }
        memcpy(c->patterns, cached, n * sizeof(db_blacklist_t));
        c->pattern_count = n;
        am_fetch_admins(c);
        return;
    }
    if (c->am->cfg.db == NULL || c->am->cfg.db->submit == NULL) {
        am_fetch_admins(c);
        return;
    }
    db_job_t job = {.kind = DB_JOB_BLACKLIST_LIST, .id = c->msg.chat.id};
    if (c->am->cfg.db->submit(c->am->cfg.db->ctx, &job, on_am_blacklist, c) != 0) {
        am_fetch_admins(c);
    }
}

/**
 * @brief Settings-fetch completion: copy the policy and load the blacklist.
 *
 * @param[in] settings Chat policy.
 * @param[in] status   Lookup status.
 * @param[in] user     The @ref am_ctx_t.
 */
static void on_am_settings(const chat_settings_t *settings, int status, void *user)
{
    am_ctx_t *c = user;
    if (status != 0 || settings == NULL) {
        free(c);
        return;
    }
    c->settings = *settings;
    c->has_settings = 1;
    am_load_blacklist(c);
}

int automod_handle_message(automod_t *a, const tg_message_t *msg)
{
    if (a == NULL || msg == NULL) {
        return -EINVAL;
    }
    /* Group/supergroup messages only, and never service messages (SPEC 5.3). */
    int is_group =
        strcmp(msg->chat.type, "group") == 0 || strcmp(msg->chat.type, "supergroup") == 0;
    if (!is_group || msg->is_service || msg->from.id == 0) {
        return 0;
    }
    am_ctx_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return -ENOMEM;
    }
    c->am = a;
    c->msg = *msg;
    if (a->cfg.settings == NULL) {
        /* No policy source: nothing to enforce. */
        free(c);
        return 0;
    }
    if (settings_get((settings_t *)a->cfg.settings, msg->chat.id, on_am_settings, c) != 0) {
        free(c);
    }
    return 0;
}

/* --- Lifecycle ------------------------------------------------------------- */

automod_t *automod_create(const automod_config_t *cfg)
{
    if (cfg == NULL || cfg->api == NULL || cfg->clock == NULL || cfg->wall == NULL) {
        return NULL;
    }
    automod_t *a = calloc(1, sizeof(*a));
    if (a == NULL) {
        return NULL;
    }
    a->cfg = *cfg;
    a->flood = cache_create(cfg->clock, 1024);
    a->blacklist = cache_create(cfg->clock, 256);
    if (a->flood == NULL || a->blacklist == NULL) {
        cache_destroy(a->flood);
        cache_destroy(a->blacklist);
        free(a);
        return NULL;
    }
    return a;
}

void automod_destroy(automod_t *a)
{
    if (a == NULL) {
        return;
    }
    cache_destroy(a->flood);
    cache_destroy(a->blacklist);
    free(a);
}
