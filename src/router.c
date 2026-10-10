/**
 * @file router.c
 * @brief Update -> command dispatch (SPEC section 5.2, 5.3).
 *
 * @ingroup router
 */
#include "tg/router.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "commands.h"
#include "tables.h"
#include "tg/cmd_ctx.h"
#include "tg/db.h"
#include "tg/errors.h"
#include "tg/perms.h"
#include "tg/tg_api.h"
#include "tg/tg_types.h"
#include "tg/util.h"

/** @brief The router state. */
struct router {
    router_config_t cfg; /**< Copied configuration. */
};

/* --- Pure parsing ---------------------------------------------------------- */

/**
 * @brief Copy @p s into @p dst, lowercased, up to @p cap-1 bytes.
 *
 * @param[out] dst Destination buffer.
 * @param[in]  cap Capacity of @p dst.
 * @param[in]  s   Source bytes.
 * @param[in]  n   Number of source bytes to consider.
 */
static void lower_copy(char *dst, size_t cap, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && i + 1 < cap; ++i) {
        dst[i] = (char)tolower((unsigned char)s[i]);
    }
    dst[i] = '\0';
}

/**
 * @brief Try to consume a leading target token from @p rest.
 *
 * A target is an `\@username` or a token consisting entirely of digits (a numeric
 * user id). A token like "30m" is a duration, not a target, so the whole
 * whitespace-delimited token must be digits. On success @p out is filled and
 * @p rest advances past the token and any following spaces.
 *
 * @param[in,out] rest Pointer to the argument string (advanced on success).
 * @param[out]    out  Invocation to fill.
 */
static void parse_target_token(const char **rest, cmd_invocation_t *out)
{
    const char *r = *rest;
    int is_at = (*r == '@');
    if (!is_at && !isdigit((unsigned char)*r)) {
        return;
    }
    size_t n = 0;
    int all_digits = 1;
    while (r[n] != '\0' && !isspace((unsigned char)r[n]) && n < ROUTER_TARGET_MAX - 1) {
        if (!isdigit((unsigned char)r[n])) {
            all_digits = 0;
        }
        n++;
    }
    /* An @username needs more than just '@'; a numeric id must be all digits. */
    if ((is_at && n <= 1) || (!is_at && !all_digits) || n == 0) {
        return;
    }
    out->has_target = 1;
    (void)snprintf(out->target, sizeof(out->target), "%.*s", (int)n, r);
    r += n;
    while (*r == ' ' || *r == '\t') {
        r++;
    }
    out->args = r;
    *rest = r;
}

/**
 * @brief Consume a `/cmd@Bot` suffix.
 *
 * @param[in]  rest         Points at the '@' (or the text after the command name).
 * @param[in]  bot_username The bot's own username, or NULL to accept any.
 * @param[out] other_bot    Set to 1 if the suffix names a different bot.
 *
 * @return The pointer just past the suffix.
 */
static const char *consume_bot_suffix(const char *rest, const char *bot_username, int *other_bot)
{
    if (*rest != '@') {
        return rest;
    }
    const char *at = rest + 1;
    size_t bot_len = 0;
    while (at[bot_len] != '\0' && !isspace((unsigned char)at[bot_len])) {
        bot_len++;
    }
    if (bot_username != NULL &&
        (strlen(bot_username) != bot_len || strncasecmp(at, bot_username, bot_len) != 0)) {
        *other_bot = 1;
    }
    return at + bot_len;
}

int router_parse_command(const char *text, const char *bot_username, cmd_invocation_t *out)
{
    if (text == NULL || out == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->args = "";

    if (text[0] != '/') {
        return 0; /* not a command */
    }

    const char *p = text + 1;
    size_t name_len = 0;
    while (p[name_len] != '\0' && !isspace((unsigned char)p[name_len]) && p[name_len] != '@') {
        name_len++;
    }
    if (name_len == 0 || name_len > ROUTER_CMD_NAME_MAX) {
        return 0;
    }
    lower_copy(out->name, sizeof(out->name), p, name_len);

    const char *rest = consume_bot_suffix(p + name_len, bot_username, &out->addressed_to_other_bot);
    if (out->addressed_to_other_bot) {
        return 1; /* it is a command, but for someone else */
    }

    /* Skip any separator space; the remainder is the argument string. */
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    out->args = rest;

    parse_target_token(&rest, out);
    return 1;
}

int router_resolve_target(const cmd_invocation_t *inv, const tg_message_t *msg, int64_t *out)
{
    if (inv == NULL || msg == NULL || out == NULL) {
        return -EINVAL;
    }
    *out = 0;

    if (msg->has_reply && msg->reply_from.id != 0) {
        *out = msg->reply_from.id;
        return 0;
    }
    if (!inv->has_target) {
        return 1; /* no target given */
    }
    if (inv->target[0] == '@') {
        return -ENOTSUP; /* \@username lookup lands with the moderation commands (M4) */
    }
    char *end = NULL;
    long long id = strtoll(inv->target, &end, 10);
    if (end == inv->target || *end != '\0' || id <= 0) {
        return -EINVAL;
    }
    *out = (int64_t)id;
    return 0;
}

/* --- DB upserts ------------------------------------------------------------ */

/**
 * @brief No-op completion for fire-and-forget upserts.
 *
 * @param[in] res DB result.
 * @param[in] ctx Unused.
 */
static void on_upsert(const db_result_t *res, void *ctx)
{
    (void)ctx;
    if (res->status != 0) {
        util_log(LOG_WARN, "router", "db upsert failed: %d", res->status);
    }
}

/**
 * @brief Upsert the message sender and its chat (fire and forget).
 *
 * @param[in] r   Router.
 * @param[in] msg Message whose sender/chat to persist.
 */
static void upsert_message(router_t *r, const tg_message_t *msg)
{
    if (r->cfg.db == NULL || r->cfg.db->submit == NULL) {
        return;
    }
    if (msg->from.id != 0) {
        db_job_t job = {.kind = DB_JOB_UPSERT_USER, .id = msg->from.id, .last_seen = msg->date};
        (void)snprintf(job.username, sizeof(job.username), "%s", msg->from.username);
        (void)snprintf(job.first_name, sizeof(job.first_name), "%s", msg->from.first_name);
        (void)r->cfg.db->submit(r->cfg.db->ctx, &job, on_upsert, NULL);
    }
    /* Also remember the replied-to message's sender: the bot learns a user's id
     * from any message it can see, including the one being replied to, which makes
     * `@username` targets resolvable even before that user speaks directly. */
    if (msg->has_reply && msg->reply_from.id != 0 && msg->reply_from.id != msg->from.id) {
        db_job_t job = {
            .kind = DB_JOB_UPSERT_USER, .id = msg->reply_from.id, .last_seen = msg->date};
        (void)snprintf(job.username, sizeof(job.username), "%s", msg->reply_from.username);
        (void)snprintf(job.first_name, sizeof(job.first_name), "%s", msg->reply_from.first_name);
        (void)r->cfg.db->submit(r->cfg.db->ctx, &job, on_upsert, NULL);
    }
    if (msg->chat.id != 0) {
        db_job_t job = {.kind = DB_JOB_UPSERT_CHAT, .id = msg->chat.id};
        (void)snprintf(job.title, sizeof(job.title), "%s", msg->chat.title);
        (void)snprintf(job.chat_type, sizeof(job.chat_type), "%s", msg->chat.type);
        (void)r->cfg.db->submit(r->cfg.db->ctx, &job, on_upsert, NULL);
    }
}

/**
 * @brief Seed the `users` cache from an administrator list (fire and forget).
 *
 * `getChatAdministrators` already carries each admin's username and name, so the
 * bot can resolve `@username` targets for admins without waiting for them to
 * speak. Only users with a username are stored (a username is the lookup key).
 *
 * @param[in] r      Router.
 * @param[in] admins Administrator list (may be NULL).
 */
static void seed_admins(router_t *r, const perms_admins_t *admins)
{
    if (admins == NULL || r->cfg.db == NULL || r->cfg.db->submit == NULL) {
        return;
    }
    for (unsigned i = 0; i < admins->count; ++i) {
        const perms_admin_t *a = &admins->admins[i];
        if (a->user_id == 0 || a->username[0] == '\0') {
            continue;
        }
        db_job_t job = {.kind = DB_JOB_UPSERT_USER, .id = a->user_id};
        (void)snprintf(job.username, sizeof(job.username), "%s", a->username);
        (void)snprintf(job.first_name, sizeof(job.first_name), "%s", a->first_name);
        (void)r->cfg.db->submit(r->cfg.db->ctx, &job, on_upsert, NULL);
    }
}

/* --- Dispatch -------------------------------------------------------------- */

/** @brief Context for the asynchronous dispatch chain. */
typedef struct dispatch {
    router_t *router;     /**< Router. */
    cmd_call_t *call;     /**< Reference-counted invocation. */
    cmd_invocation_t inv; /**< Parsed invocation. */
    int is_group;         /**< 1 if the chat is a group/supergroup. */
} dispatch_t;

/**
 * @brief Fill the invocation's role-derived fields and run the command handler.
 *
 * @param[in] d      Dispatch context.
 * @param[in] admins Administrator list (may be NULL for private chats).
 */
static void run_handler(dispatch_t *d, const perms_admins_t *admins)
{
    const cmd_desc_t *cmd = cmd_lookup(d->inv.name);
    if (cmd == NULL) {
        return;
    }
    cmd_call_t *call = d->call;
    struct cmd_ctx *ctx = cmd_call_ctx(call);
    tg_message_t *msg = cmd_call_message(call);

    perms_subject_t subject;
    (void)perms_subject_from_admins(d->router->cfg.perms, admins, ctx->user_id, &subject);

    /* Seed the username cache from the admin list the fetch already returned. */
    seed_admins(d->router, admins);

    ctx->cmd = cmd;
    ctx->chat_id = msg->chat.id;
    ctx->user_id = msg->from.id;
    ctx->bot_id = d->router->cfg.bot_id;
    ctx->is_group = d->is_group;
    ctx->role = subject.role;
    ctx->is_sudo = subject.is_sudo;
    ctx->args = d->inv.args;
    ctx->target_role = perms_role_of(admins, ctx->target_id);
    ctx->bot_has_right = perms_bot_has_right(admins, d->router->cfg.bot_id, cmd->bot_right);
    ctx->wall = d->router->cfg.wall;

    /* A human-readable label for replies: the reply sender's name when the target
     * came from a reply, otherwise the \@username / numeric token. */
    if (ctx->target_id != 0) {
        if (msg->has_reply && msg->reply_from.id == ctx->target_id &&
            msg->reply_from.first_name[0] != '\0') {
            (void)cmd_call_set_target_label(call, msg->reply_from.first_name);
        } else if (d->inv.target[0] != '\0') {
            (void)cmd_call_set_target_label(call, d->inv.target);
        }
    }

    int hrc = cmd->handler(ctx);
    if (hrc != 0) {
        util_log(LOG_WARN, "router", "command '%s' returned %d", cmd->name, hrc);
    }
}

/**
 * @brief Reply with a permission denial chosen from the error class.
 *
 * @param[in] ctx Context.
 * @param[in] rc  The permission error (-ERR_TG_API or -ERR_PERM).
 */
static void reply_denied(struct cmd_ctx *ctx, int rc)
{
    int64_t reply_to = ctx->msg != NULL ? ctx->msg->message_id : 0;
    (void)cmd_ctx_reply_plain(ctx, reply_to,
                              rc == -ERR_TG_API ? "I lack the rights to do that in this chat."
                                                : "You are not allowed to use that command.");
}

/**
 * @brief Gate on permissions and run the command handler.
 *
 * @param[in] d      Dispatch context.
 * @param[in] admins Administrator list (may be NULL for private chats).
 */
static void dispatch_subject(dispatch_t *d, const perms_admins_t *admins)
{
    const cmd_desc_t *cmd = cmd_lookup(d->inv.name);
    if (cmd == NULL) {
        return; /* unknown command: ignore silently (another bot's, or a typo) */
    }
    struct cmd_ctx *ctx = cmd_call_ctx(d->call);

    /* Group-only commands are rejected in private chats (SPEC section 5.2). */
    if (!d->is_group && (cmd->flags & CMDF_GROUP_ONLY) != 0u) {
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "That command is only available in groups.");
        return;
    }

    int bot_has = perms_bot_has_right(admins, d->router->cfg.bot_id, cmd->bot_right);
    perms_subject_t subject;
    (void)perms_subject_from_admins(d->router->cfg.perms, admins, ctx->user_id, &subject);
    int rc = perms_check(&subject, cmd, bot_has);
    if (rc != 0) {
        reply_denied(ctx, rc);
        return;
    }
    run_handler(d, admins);
}

/**
 * @brief Release the router's reference to the invocation and free the dispatch.
 *
 * Called on every terminal dispatch path (a reply was sent, the handler ran, or
 * the command was denied); the invocation itself is freed once its in-flight
 * requests also release their references.
 *
 * @param[in] d Dispatch context.
 */
static void finish_dispatch(dispatch_t *d)
{
    cmd_call_release(d->call);
    free(d);
}

/**
 * @brief Start the admin fetch (group) or dispatch with no list (private chat).
 *
 * @param[in] d Dispatch context.
 */
static void dispatch_with_admins(dispatch_t *d);

/** @brief \@username-lookup completion (defined below). */
static void on_username_resolved(const db_result_t *res, void *user);

/**
 * @brief Look up an \@username in an administrator list (case-insensitive).
 *
 * Lets `@username` targets resolve immediately for anyone in the admin list the
 * fetch already returned, without waiting for the users-cache write to land.
 *
 * @param[in] admins   Administrator list (may be NULL).
 * @param[in] username Username without the leading '@'.
 *
 * @return The matching user id, or 0 if not found.
 */
static int64_t username_in_admins(const perms_admins_t *admins, const char *username)
{
    if (admins == NULL) {
        return 0;
    }
    for (unsigned i = 0; i < admins->count; ++i) {
        if (admins->admins[i].username[0] != '\0' &&
            strcasecmp(admins->admins[i].username, username) == 0) {
            return admins->admins[i].user_id;
        }
    }
    return 0;
}

/**
 * @brief Resolve the target (seeding the username cache first), then dispatch.
 *
 * Runs after the administrator list is known, so `@username` targets can be
 * resolved from admins the fetch already named. A reply-to or numeric target is
 * set synchronously; an `@username` is first matched against the admin list in
 * memory, then looked up in the users cache (DB), re-entering
 * @ref dispatch_with_admins when it completes.
 *
 * @param[in] d      Dispatch context.
 * @param[in] admins Administrator list (may be NULL for private chats).
 */
static void dispatch_after_admins(dispatch_t *d, const perms_admins_t *admins)
{
    struct cmd_ctx *ctx = cmd_call_ctx(d->call);
    const tg_message_t *copy = cmd_call_message(d->call);

    /* Seed the users cache from the admin list for later commands. */
    seed_admins(d->router, admins);

    /* A previous pass may already have resolved the target (an \@username DB
     * lookup sets ctx->target_id and re-enters here); do not resolve again. */
    if (ctx->target_id != 0) {
        dispatch_subject(d, admins);
        finish_dispatch(d);
        return;
    }

    int64_t target = 0;
    int resolved = router_resolve_target(&d->inv, copy, &target);
    if (resolved == 0) {
        ctx->target_id = target;
        dispatch_subject(d, admins);
        finish_dispatch(d);
        return;
    }
    if (resolved != -ENOTSUP) {
        dispatch_subject(d, admins); /* no target: the handler reports it */
        finish_dispatch(d);
        return;
    }

    /* An \@username: resolve against the admin list first (no DB round-trip), then
     * fall back to the users cache. */
    const char *uname = d->inv.target + 1;
    int64_t admin_id = username_in_admins(admins, uname);
    if (admin_id != 0) {
        ctx->target_id = admin_id;
        dispatch_subject(d, admins);
        finish_dispatch(d);
        return;
    }
    if (d->router->cfg.db == NULL || d->router->cfg.db->submit == NULL) {
        (void)cmd_ctx_reply_plain(ctx, copy->message_id, "I cannot resolve usernames right now.");
        finish_dispatch(d);
        return;
    }
    db_job_t job = {.kind = DB_JOB_FIND_USER_BY_USERNAME};
    (void)snprintf(job.username, sizeof(job.username), "%s", uname);
    if (d->router->cfg.db->submit(d->router->cfg.db->ctx, &job, on_username_resolved, d) != 0) {
        finish_dispatch(d);
    }
}

/**
 * @brief Admin-fetch completion: seed and dispatch, or report the failure.
 *
 * @param[in] admins Administrator list (NULL on failure).
 * @param[in] status Fetch status.
 * @param[in] user   The @ref dispatch_t.
 */
static void on_admins(const perms_admins_t *admins, int status, void *user)
{
    dispatch_t *d = user;
    if (status != 0) {
        struct cmd_ctx *ctx = cmd_call_ctx(d->call);
        util_log(LOG_WARN, "router", "permission check failed: %d", status);
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "I could not verify permissions in this chat; please try again.");
        finish_dispatch(d);
        return;
    }
    dispatch_after_admins(d, admins);
}

/**
 * @brief Start the admin fetch (group) or dispatch with no list (private chat).
 *
 * @param[in] d Dispatch context.
 */
static void dispatch_with_admins(dispatch_t *d)
{
    struct cmd_ctx *ctx = cmd_call_ctx(d->call);
    if (d->router->cfg.perms == NULL || !d->is_group) {
        dispatch_after_admins(d, NULL);
        return;
    }
    if (perms_admins_get(d->router->cfg.perms, ctx->chat_id, on_admins, d) != 0) {
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "I could not verify permissions in this chat; please try again.");
        finish_dispatch(d);
    }
}

/**
 * @brief \@username-lookup completion: set the target and re-dispatch.
 *
 * @param[in] res DB result (its @c i64 is the resolved user id, 0 if unknown).
 * @param[in] user The @ref dispatch_t.
 */
static void on_username_resolved(const db_result_t *res, void *user)
{
    dispatch_t *d = user;
    struct cmd_ctx *ctx = cmd_call_ctx(d->call);
    if (res->status == 0 && res->i64 != 0) {
        ctx->target_id = res->i64;
        /* Re-run the dispatch; the admin list is now a cache hit, and the target
         * resolves synchronously the second time. */
        dispatch_with_admins(d);
        return;
    }
    if (res->status == 0) {
        (void)cmd_ctx_reply_plain(
            ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
            "I do not know that username yet. Reply to one of their messages, or use their "
            "numeric user id.");
    }
    finish_dispatch(d);
}

/**
 * @brief Handle a message update: upsert, parse, resolve target, gate, dispatch.
 *
 * @param[in] r   Router.
 * @param[in] upd Message update to handle.
 *
 * @return 0 if handled or ignored, negative errno on a fatal error.
 */
static int handle_message(router_t *r, const tg_update_t *upd)
{
    const tg_message_t *msg = &upd->message;

    /* Upsert the sender/chat for every non-service message. */
    upsert_message(r, msg);

    if (msg->is_service || msg->from.is_bot || msg->text[0] == '\0') {
        return 0; /* service messages, bots and non-text are not commands */
    }

    cmd_invocation_t inv;
    int is_cmd = router_parse_command(msg->text, r->cfg.bot_username, &inv);
    if (is_cmd <= 0 || inv.addressed_to_other_bot) {
        return 0;
    }

    cmd_call_t *call = cmd_call_create(r->cfg.api, r->cfg.db, r->cfg.wall);
    if (call == NULL) {
        return -ENOMEM;
    }
    tg_message_t *copy = cmd_call_message(call);
    *copy = *msg;

    dispatch_t *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        cmd_call_release(call);
        return -ENOMEM;
    }
    d->router = r;
    d->call = call;
    d->inv = inv;
    /* The invocation was parsed from the borrowed update text; re-point @args into
     * the copied message so it stays valid across the asynchronous dispatch. */
    if (inv.args >= msg->text && inv.args <= msg->text + strlen(msg->text)) {
        d->inv.args = copy->text + (inv.args - msg->text);
    }
    d->is_group =
        strcmp(copy->chat.type, "group") == 0 || strcmp(copy->chat.type, "supergroup") == 0;

    /* Fill the identity fields up front: the admin fetch below reads chat_id, and
     * every reply/record path needs chat_id/user_id/bot_id regardless of how the
     * dispatch chain resolves the caller's role. */
    struct cmd_ctx *ctx = cmd_call_ctx(call);
    ctx->chat_id = copy->chat.id;
    ctx->user_id = copy->from.id;
    ctx->bot_id = r->cfg.bot_id;
    ctx->is_group = d->is_group;
    ctx->args = d->inv.args;
    ctx->wall = r->cfg.wall;

    /* Resolve the target and dispatch. Target resolution (including the \@username
     * DB lookup) runs after the admin fetch so the users cache is seeded first. */
    dispatch_with_admins(d);
    return 0;
}

/* --- Public API ------------------------------------------------------------ */

router_t *router_create(const router_config_t *cfg)
{
    if (cfg == NULL || cfg->api == NULL || cfg->api->request == NULL) {
        return NULL;
    }
    router_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->cfg = *cfg;
    return r;
}

int router_handle(router_t *r, const tg_update_t *upd)
{
    if (r == NULL || upd == NULL) {
        return -EINVAL;
    }
    switch (upd->kind) {
    case TG_UPD_MESSAGE:
    case TG_UPD_EDITED_MESSAGE:
        return handle_message(r, upd);
    case TG_UPD_CHAT_MEMBER:
    case TG_UPD_MY_CHAT_MEMBER:
        /* A membership change invalidates the cached admin list (SPEC 5.1). */
        if (r->cfg.perms != NULL) {
            perms_invalidate(r->cfg.perms, upd->member_chat.id);
        }
        return 0;
    case TG_UPD_CALLBACK_QUERY: /* Captcha callbacks arrive in M6. */
    case TG_UPD_UNKNOWN:
    default:
        return 0;
    }
}

void router_destroy(router_t *r)
{
    free(r);
}
