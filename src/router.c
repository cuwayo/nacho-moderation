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

#include "api_methods.h"
#include "commands.h"
#include "tables.h"
#include "tg/api_pool.h"
#include "tg/automod.h"
#include "tg/captcha.h"
#include "tg/cmd_ctx.h"
#include "tg/cmd_settings.h"
#include "tg/db.h"
#include "tg/errors.h"
#include "tg/perms.h"
#include "tg/settings.h"
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

int router_parse_command_ex(const char *text, const char *bot_username, int want_target,
                            cmd_invocation_t *out)
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

    /* Only commands that take a target (CMDF_TARGET) consume a leading token; a
     * numeric argument such as `/setwarnlimit 5` must stay in @c args. */
    if (want_target) {
        parse_target_token(&rest, out);
    }
    return 1;
}

int router_parse_command(const char *text, const char *bot_username, cmd_invocation_t *out)
{
    return router_parse_command_ex(text, bot_username, 1, out);
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

/* --- Dispatch -------------------------------------------------------------- */

/** @brief Deliver the join welcome if the chat has it enabled (defined below). */
static void maybe_welcome(router_t *r, const tg_chat_t *chat, const tg_user_t *user);

/** @brief Context for the asynchronous dispatch chain. */
typedef struct dispatch {
    router_t *router;     /**< Router. */
    cmd_call_t *call;     /**< Reference-counted invocation. */
    cmd_invocation_t inv; /**< Parsed invocation. */
    int is_group;         /**< 1 if the chat is a group/supergroup. */
} dispatch_t;

/**
 * @brief Adapter for @ref cmd_call_set_admin_invalidator (drops the perms cache).
 *
 * @param[in] impl    The @ref perms_t.
 * @param[in] chat_id Chat whose cached admin list is dropped.
 */
static void perms_invalidate_adapter(void *impl, int64_t chat_id)
{
    perms_invalidate(impl, chat_id);
}

/**
 * @brief Adapter for @ref cmd_call_set_settings_invalidator (drops the settings cache).
 *
 * @param[in] impl    The @ref settings_t.
 * @param[in] chat_id Chat whose cached settings are dropped.
 */
static void settings_invalidate_adapter(void *impl, int64_t chat_id)
{
    settings_invalidate(impl, chat_id);
}

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
 * @brief Settings-lookup completion: store the policy and continue to the admins.
 *
 * @param[in] settings Policy (NULL on failure).
 * @param[in] status   Lookup status.
 * @param[in] user     The @ref dispatch_t.
 */
static void on_settings(const chat_settings_t *settings, int status, void *user)
{
    dispatch_t *d = user;
    struct cmd_ctx *ctx = cmd_call_ctx(d->call);
    if (status != 0) {
        util_log(LOG_WARN, "router", "settings lookup failed: %d", status);
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "I could not load this chat's settings; please try again.");
        finish_dispatch(d);
        return;
    }
    (void)cmd_ctx_set_settings(ctx, settings);
    dispatch_with_admins(d);
}

/**
 * @brief Load the chat's settings, then continue the dispatch chain.
 *
 * Settings are fetched before the administrator list because the admin list
 * pointer is only valid for the duration of its callback, whereas settings are
 * copied into the invocation. Private chats have no per-chat settings, so they
 * continue straight to @ref dispatch_with_admins.
 *
 * @param[in] d Dispatch context.
 */
static void dispatch_with_settings(dispatch_t *d)
{
    struct cmd_ctx *ctx = cmd_call_ctx(d->call);
    if (d->router->cfg.settings == NULL || !d->is_group) {
        dispatch_with_admins(d);
        return;
    }
    if (settings_get(d->router->cfg.settings, ctx->chat_id, on_settings, d) != 0) {
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "I could not load this chat's settings; please try again.");
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

    /* Automated moderation runs on every group message, before command dispatch
     * (SPEC section 5.3): locks -> blacklist -> antiflood. It may delete the
     * message, so it runs before the command path but never blocks it. */
    if (r->cfg.automod != NULL) {
        (void)automod_handle_message(r->cfg.automod, msg);
    }

    /* Basic-group joins arrive as a service message with new_chat_members (SPEC
     * section 5.4); supergroup joins arrive as chat_member. Greet each joiner
     * (through the captcha flow when it is enabled). */
    for (unsigned i = 0; i < msg->new_member_count; ++i) {
        maybe_welcome(r, &msg->chat, &msg->new_members[i]);
    }

    if (msg->is_service || msg->from.is_bot || msg->text[0] == '\0') {
        return 0; /* service messages, bots and non-text are not commands */
    }

    /* Peek at the command name to learn whether it takes a target, so a numeric
     * argument (`/setwarnlimit 5`) is not consumed as a target token. */
    cmd_invocation_t peek;
    int is_cmd = router_parse_command_ex(msg->text, r->cfg.bot_username, 0, &peek);
    if (is_cmd <= 0 || peek.addressed_to_other_bot) {
        return 0;
    }
    const cmd_desc_t *cmd = cmd_lookup(peek.name);
    if (cmd == NULL) {
        return 0; /* unknown command: ignore silently */
    }
    int want_target = (cmd->flags & CMDF_TARGET) != 0;

    cmd_invocation_t inv;
    if (router_parse_command_ex(msg->text, r->cfg.bot_username, want_target, &inv) <= 0) {
        return 0;
    }

    cmd_call_t *call = cmd_call_create(r->cfg.api, r->cfg.db, r->cfg.wall);
    if (call == NULL) {
        return -ENOMEM;
    }
    cmd_call_set_admin_invalidator(call, perms_invalidate_adapter, r->cfg.perms);
    cmd_call_set_settings_invalidator(call, settings_invalidate_adapter, r->cfg.settings);
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

    /* Fill the identity fields up front: the settings/admin fetch below reads
     * chat_id, and every reply/record path needs chat_id/user_id/bot_id regardless
     * of how the dispatch chain resolves the caller's role. */
    struct cmd_ctx *ctx = cmd_call_ctx(call);
    ctx->chat_id = copy->chat.id;
    ctx->user_id = copy->from.id;
    ctx->bot_id = r->cfg.bot_id;
    ctx->is_group = d->is_group;
    ctx->args = d->inv.args;
    ctx->wall = r->cfg.wall;

    /* Load settings, then admins, then resolve the target and dispatch. */
    dispatch_with_settings(d);
    return 0;
}

/* --- Inline-keyboard callbacks --------------------------------------------- */

/** @brief Context for the settings-panel callback chain. */
typedef struct {
    router_t *router;     /**< Router. */
    cmd_call_t *call;     /**< Reference-counted invocation. */
    int64_t chat_id;      /**< Chat the panel lives in. */
    int64_t owner_id;     /**< User the panel belongs to (callback ownership). */
    int64_t message_id;   /**< Panel message id (for editMessageText). */
    char callback_id[64]; /**< Query id (for answerCallbackQuery). */
    int new_enabled;      /**< Value to write for the welcome toggle. */
} cb_ctx_t;

/**
 * @brief No-op completion for fire-and-forget API calls in the router.
 *
 * @param[in] res API result (ignored).
 * @param[in] user Unused.
 */
static void on_ignore_api(const api_result_t *res, void *user)
{
    (void)res;
    (void)user;
}

/**
 * @brief Answer a callback query so the client stops showing a spinner.
 *
 * @param[in] r    Router.
 * @param[in] cb   Callback context.
 * @param[in] text Toast text (may be NULL for none).
 */
static void answer_callback(router_t *r, const cb_ctx_t *cb, const char *text)
{
    char body[256];
    int n;
    if (text != NULL) {
        n = snprintf(body, sizeof(body), "{\"callback_query_id\":\"%s\",\"text\":\"%s\"}",
                     cb->callback_id, text);
    } else {
        n = snprintf(body, sizeof(body), "{\"callback_query_id\":\"%s\"}", cb->callback_id);
    }
    if (n > 0 && (size_t)n < sizeof(body)) {
        (void)r->cfg.api->request(r->cfg.api->ctx, METHOD_ANSWER_CALLBACK_QUERY, body, (size_t)n,
                                  cb->chat_id, 0, on_ignore_api, NULL);
    }
}

/**
 * @brief Build the inline-keyboard `reply_markup` for the settings panel.
 *
 * @param[out] out     Destination buffer.
 * @param[in]  cap     Capacity of @p out.
 * @param[in]  chat_id Chat id embedded in the callback data.
 * @param[in]  user_id Owner of the panel (the callback is rejected for others).
 * @param[in]  enabled 1 if the welcome is currently enabled.
 *
 * @return 0 on success, -ENOSPC if @p out is too small.
 */
static int build_panel_markup(char *out, size_t cap, int64_t chat_id, int64_t user_id, int enabled)
{
    const char *label = enabled != 0 ? "Welcome: ON" : "Welcome: OFF";
    int n = snprintf(out, cap,
                     "\"reply_markup\":{\"inline_keyboard\":[[{\"text\":\"%s\","
                     "\"callback_data\":\"set:%lld:%lld:welcome\"}]]}",
                     label, (long long)chat_id, (long long)user_id);
    return (n < 0 || (size_t)n >= cap) ? -ENOSPC : 0;
}

/**
 * @brief Edit the settings-panel message to reflect the new toggle state.
 *
 * @param[in] cb The callback context (carries the new state).
 */
static void edit_panel(cb_ctx_t *cb)
{
    char markup[256];
    if (build_panel_markup(markup, sizeof(markup), cb->chat_id, cb->owner_id, cb->new_enabled) !=
        0) {
        return;
    }
    char body[512];
    int n = snprintf(body, sizeof(body),
                     "{\"chat_id\":%lld,\"message_id\":%lld,\"text\":\"Chat settings\","
                     "\"parse_mode\":\"HTML\",%s}",
                     (long long)cb->chat_id, (long long)cb->message_id, markup);
    if (n > 0 && (size_t)n < sizeof(body)) {
        (void)cmd_ctx_request(cmd_call_ctx(cb->call), METHOD_EDIT_MESSAGE_TEXT, body, (size_t)n,
                              NULL, NULL);
    }
}

/**
 * @brief DB completion for the panel toggle: edit the panel and release the call.
 *
 * @param[in] res DB result.
 * @param[in] user The @ref cb_ctx_t.
 */
static void on_panel_write(const db_result_t *res, void *user)
{
    cb_ctx_t *cb = user;
    if (res->status == 0) {
        edit_panel(cb);
    }
    cmd_call_release(cb->call);
    free(cb);
}

/**
 * @brief Settings-fetch completion for a panel press: toggle and persist.
 *
 * @param[in] settings Current policy.
 * @param[in] status   Lookup status.
 * @param[in] user     The @ref cb_ctx_t.
 */
static void on_panel_settings(const chat_settings_t *settings, int status, void *user)
{
    cb_ctx_t *cb = user;
    struct cmd_ctx *ctx = cmd_call_ctx(cb->call);
    if (status != 0 || settings == NULL) {
        answer_callback(cb->router, cb, "Could not read settings.");
        cmd_call_release(cb->call);
        free(cb);
        return;
    }
    cb->new_enabled = settings->welcome_enabled != 0 ? 0 : 1;
    answer_callback(cb->router, cb,
                    cb->new_enabled != 0 ? "Welcome enabled." : "Welcome disabled.");
    if (ctx->db_submit == NULL) {
        cmd_call_release(cb->call);
        free(cb);
        return;
    }
    db_job_t job = {.kind = DB_JOB_SETTINGS_SET,
                    .id = cb->chat_id,
                    .settings_mask = DB_SET_WELCOME_ON,
                    .value = cb->new_enabled};
    if (ctx->db_submit(ctx, &job, on_panel_write, cb) != 0) {
        cmd_call_release(cb->call);
        free(cb);
        return;
    }
    if (ctx->invalidate_settings != NULL) {
        ctx->invalidate_settings(ctx);
    }
}

/**
 * @brief Admin-fetch completion for a panel press: check the right, then toggle.
 *
 * @param[in] admins Administrator list (NULL on failure).
 * @param[in] status Fetch status.
 * @param[in] user   The @ref cb_ctx_t.
 */
static void on_panel_admins(const perms_admins_t *admins, int status, void *user)
{
    cb_ctx_t *cb = user;
    const struct cmd_ctx *ctx = cmd_call_ctx(cb->call);
    const cmd_desc_t *cmd = cmd_lookup("settings");
    perms_subject_t subject;
    (void)perms_subject_from_admins(cb->router->cfg.perms, admins, ctx->user_id, &subject);
    int bot_has = perms_bot_has_right(admins, cb->router->cfg.bot_id, cmd->bot_right);
    if (status != 0 || perms_check(&subject, cmd, bot_has) != 0) {
        answer_callback(cb->router, cb, "You are not allowed to change settings.");
        cmd_call_release(cb->call);
        free(cb);
        return;
    }
    if (settings_get(cb->router->cfg.settings, cb->chat_id, on_panel_settings, cb) != 0) {
        cmd_call_release(cb->call);
        free(cb);
    }
}

/**
 * @brief Handle a settings-panel button press (SPEC sections 5.2, 10).
 *
 * Enforces the callback-ownership rule before doing anything: the presser's user
 * id must equal the id baked into the payload. Then it resolves the presser's
 * permissions and toggles the welcome.
 *
 * @param[in] r   Router.
 * @param[in] upd Callback-query update.
 *
 * @return 0 if handled, negative errno on a fatal error.
 */
static int handle_callback(router_t *r, const tg_update_t *upd)
{
    /* A captcha "I'm human" button press (SPEC section 5.4) is handled by the
     * captcha service before the settings panel is considered. */
    if (r->cfg.captcha != NULL && captcha_handle_callback(r->cfg.captcha, upd) != 0) {
        return 0;
    }
    int64_t chat = 0;
    int64_t owner = 0;
    const char *key = NULL;
    if (cmd_settings_parse_callback(upd->callback_data, &chat, &owner, &key) != 0) {
        return 0; /* not a settings callback */
    }
    if (!upd->callback_has_message) {
        return 0; /* nothing to edit */
    }

    cmd_call_t *call = cmd_call_create(r->cfg.api, r->cfg.db, r->cfg.wall);
    if (call == NULL) {
        return -ENOMEM;
    }
    cmd_call_set_admin_invalidator(call, perms_invalidate_adapter, r->cfg.perms);
    cmd_call_set_settings_invalidator(call, settings_invalidate_adapter, r->cfg.settings);

    /* Synthesize the message the reply helpers expect (chat + message id only). */
    tg_message_t *msg = cmd_call_message(call);
    msg->chat.id = upd->callback_chat.id;
    (void)snprintf(msg->chat.type, sizeof(msg->chat.type), "%s", upd->callback_chat.type);
    msg->message_id = upd->callback_message_id;

    struct cmd_ctx *ctx = cmd_call_ctx(call);
    ctx->chat_id = upd->callback_chat.id;
    ctx->user_id = upd->callback_from.id;
    ctx->bot_id = r->cfg.bot_id;
    ctx->callback_query_id = upd->callback_id;
    ctx->is_group = strcmp(upd->callback_chat.type, "group") == 0 ||
                    strcmp(upd->callback_chat.type, "supergroup") == 0;
    ctx->wall = r->cfg.wall;

    cb_ctx_t *cb = calloc(1, sizeof(*cb));
    if (cb == NULL) {
        cmd_call_release(call);
        return -ENOMEM;
    }
    cb->router = r;
    cb->call = call;
    cb->chat_id = upd->callback_chat.id;
    cb->owner_id = owner;
    cb->message_id = upd->callback_message_id;
    (void)snprintf(cb->callback_id, sizeof(cb->callback_id), "%s", upd->callback_id);

    /* Ownership (SPEC section 10): only the user the panel was opened for may use
     * its buttons, so a forwarded message cannot drive someone else's panel. */
    if (upd->callback_from.id != owner) {
        answer_callback(r, cb, "This panel is not yours.");
        cmd_call_release(call);
        free(cb);
        return 0;
    }
    if (strcmp(key, "welcome") != 0) {
        answer_callback(r, cb, "Unknown setting.");
        cmd_call_release(call);
        free(cb);
        return 0;
    }

    if (r->cfg.perms == NULL || !ctx->is_group) {
        /* Without a permission engine we cannot verify the right; refuse safely. */
        answer_callback(r, cb, "You are not allowed to change settings.");
        cmd_call_release(call);
        free(cb);
        return 0;
    }
    if (perms_admins_get(r->cfg.perms, ctx->chat_id, on_panel_admins, cb) != 0) {
        answer_callback(r, cb, "Could not verify permissions.");
        cmd_call_release(call);
        free(cb);
    }
    return 0;
}

/* --- Join -> welcome ------------------------------------------------------- */

/**
 * @brief Render and send the join welcome for @p user in @p chat.
 *
 * Fetches the member count for `{count}`, renders the template and sends it with
 * `sendMessage`. Factored so M6's captcha flow can call it after a member passes
 * the challenge (SPEC section 5.4).
 *
 * @param[in] r       Router.
 * @param[in] chat    Chat the user joined.
 * @param[in] user    Joining user.
 * @param[in] tmpl    Welcome template (already known enabled and non-empty).
 */
static void send_welcome(router_t *r, const tg_chat_t *chat, const tg_user_t *user,
                         const char *tmpl);

/** @brief getChatMemberCount completion for the welcome `{count}`. */
typedef struct {
    router_t *router;       /**< Router. */
    tg_chat_t chat;         /**< Chat joined. */
    tg_user_t user;         /**< Joining user. */
    char tmpl[DB_TEXT_MAX]; /**< Welcome template. */
} welcome_ctx_t;

/**
 * @brief getChatMemberCount completion: render and send the welcome.
 *
 * @param[in] res API result (the count is in the body, parsed here).
 * @param[in] user The @ref welcome_ctx_t.
 */
static void on_member_count(const api_result_t *res, void *user)
{
    welcome_ctx_t *w = user;
    int count = 0;
    if (res->status == 0 && res->body != NULL) {
        /* The result is a bare integer: {"ok":true,"result":42}. */
        const char *p = strstr(res->body, "\"result\":");
        if (p != NULL) {
            char *end = NULL;
            long v = strtol(p + 9, &end, 10);
            if (end != p + 9 && v > 0) {
                count = (int)v;
            }
        }
    }
    char html[DB_TEXT_MAX * 2u];
    if (cmd_render_welcome(w->tmpl, &w->user, w->chat.title, count, html, sizeof(html)) == 0) {
        char body[DB_TEXT_MAX * 3u];
        size_t len = 0;
        if (cmd_ctx_build_send_message(body, sizeof(body), w->chat.id, html, 0, &len) == 0) {
            (void)w->router->cfg.api->request(w->router->cfg.api->ctx, METHOD_SEND_MESSAGE, body,
                                              len, w->chat.id, 0, on_ignore_api, NULL);
        }
    }
    free(w);
}

static void send_welcome(router_t *r, const tg_chat_t *chat, const tg_user_t *user,
                         const char *tmpl)
{
    if (r->cfg.api == NULL || r->cfg.api->request == NULL) {
        return;
    }
    welcome_ctx_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return;
    }
    w->router = r;
    w->chat = *chat;
    w->user = *user;
    (void)snprintf(w->tmpl, sizeof(w->tmpl), "%s", tmpl);
    char body[64];
    (void)snprintf(body, sizeof(body), "{\"chat_id\":%lld}", (long long)chat->id);
    if (r->cfg.api->request(r->cfg.api->ctx, METHOD_GET_CHAT_MEMBER_COUNT, body, strlen(body),
                            chat->id, 0, on_member_count, w) != 0) {
        free(w);
    }
}

/**
 * @brief Welcome-fetch completion: deliver the welcome if enabled.
 *
 * @param[in] settings Chat policy.
 * @param[in] status   Lookup status.
 * @param[in] user     The @ref welcome_ctx_t.
 */
static void on_welcome_settings(const chat_settings_t *settings, int status, void *user)
{
    welcome_ctx_t *w = user;
    if (status == 0 && settings != NULL && settings->welcome_enabled != 0 &&
        settings->welcome_text[0] != '\0') {
        (void)snprintf(w->tmpl, sizeof(w->tmpl), "%s", settings->welcome_text);
        send_welcome(w->router, &w->chat, &w->user, w->tmpl);
    }
    free(w);
}

/**
 * @brief Deliver the welcome for a join, if the chat has it enabled.
 *
 * @param[in] r    Router.
 * @param[in] chat Chat joined.
 * @param[in] user Joining user (bots are ignored).
 */
static void deliver_welcome(router_t *r, const tg_chat_t *chat, const tg_user_t *user)
{
    if (user->is_bot || user->id == 0 || r->cfg.settings == NULL) {
        return;
    }
    welcome_ctx_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return;
    }
    w->router = r;
    w->chat = *chat;
    w->user = *user;
    if (settings_get(r->cfg.settings, chat->id, on_welcome_settings, w) != 0) {
        free(w);
    }
}

/**
 * @brief Deliver the welcome for a join, routing through the captcha when wired.
 *
 * With the captcha enabled the member is challenged instead; the captcha service
 * calls back into @ref router_on_captcha_welcome once the challenge is passed
 * (or immediately, when the captcha is disabled). Without a captcha service the
 * welcome is delivered directly (SPEC section 5.4).
 *
 * @param[in] r    Router.
 * @param[in] chat Chat joined.
 * @param[in] user Joining user (bots are ignored).
 */
static void maybe_welcome(router_t *r, const tg_chat_t *chat, const tg_user_t *user)
{
    if (user->is_bot || user->id == 0) {
        return;
    }
    if (r->cfg.captcha != NULL) {
        (void)captcha_begin(r->cfg.captcha, chat, user);
        return;
    }
    deliver_welcome(r, chat, user);
}

void router_on_captcha_welcome(void *ctx, const tg_chat_t *chat, const tg_user_t *user)
{
    deliver_welcome(ctx, chat, user);
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

/**
 * @brief Report whether a membership transition is a join (SPEC section 5.4).
 *
 * Telegram sends `chat_member` only for supergroups; a transition into `member`
 * from a non-member status is a join. Basic-group joins arrive as
 * `message.new_chat_members` instead, so the two paths never double-fire.
 *
 * @param[in] old_status Previous ChatMember status ("" if unknown).
 * @param[in] new_status New ChatMember status.
 *
 * @return 1 if this is a join, else 0.
 */
static int is_join_transition(const char *old_status, const char *new_status)
{
    if (strcmp(new_status, "member") != 0) {
        return 0;
    }
    return strcmp(old_status, "member") != 0 && strcmp(old_status, "administrator") != 0 &&
           strcmp(old_status, "creator") != 0;
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
        /* A join in a supergroup arrives here; welcome the new member. */
        if (upd->kind == TG_UPD_CHAT_MEMBER &&
            is_join_transition(upd->old_status, upd->new_status)) {
            maybe_welcome(r, &upd->member_chat, &upd->member_user);
        }
        return 0;
    case TG_UPD_CALLBACK_QUERY:
        return handle_callback(r, upd);
    case TG_UPD_UNKNOWN:
    default:
        return 0;
    }
}

void router_destroy(router_t *r)
{
    free(r);
}
