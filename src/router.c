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
#include "tg/cmd_ctx.h"
#include "tg/db.h"
#include "tg/errors.h"
#include "tg/perms.h"
#include "tg/tg_api.h"
#include "tg/tg_types.h"
#include "tg/util.h"

/**
 * @brief No-op completion for fire-and-forget API calls (replies, upserts).
 *
 * @param[in] res API result.
 * @param[in] ctx Unused.
 */
static void on_ignore_result(const api_result_t *res, void *ctx)
{
    (void)ctx;
    if (res->status != 0) {
        util_log(LOG_DEBUG, "router", "fire-and-forget call failed: %d", res->status);
    }
}

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

/* --- Reply helper ---------------------------------------------------------- */

/** @brief Context for @c router_reply, owned by the router. */
typedef struct {
    const tg_api_ops_t *api; /**< API seam. */
} router_reply_ctx_t;

/**
 * @brief Return the JSON escape for @p c, or NULL if @p c needs no escaping.
 *
 * @param[in] c Character to escape.
 *
 * @return The escape sequence, or NULL.
 */
static const char *json_escape(char c)
{
    switch (c) {
    case '"':
        return "\\\"";
    case '\\':
        return "\\\\";
    case '\n':
        return "\\n";
    case '\r':
        return "\\r";
    case '\t':
        return "\\t";
    default:
        return NULL;
    }
}

/**
 * @brief Append the JSON-escaped @p s to @p body at @p off.
 *
 * @param[out] body Destination buffer.
 * @param[in]  cap  Capacity of @p body.
 * @param[in]  off  Current write offset.
 * @param[in]  s    String to append.
 *
 * @return The new offset, or (size_t)-1 if @p body is too small.
 */
static size_t json_append(char *body, size_t cap, size_t off, const char *s)
{
    for (; *s != '\0'; ++s) {
        const char *esc = json_escape(*s);
        size_t need = esc != NULL ? strlen(esc) : 1u;
        if (off + need + 1 >= cap) {
            return (size_t)-1;
        }
        if (esc != NULL) {
            (void)memcpy(body + off, esc, need);
        } else {
            body[off] = *s;
        }
        off += need;
        body[off] = '\0';
    }
    return off;
}

/**
 * @brief Send an HTML reply through the API seam (the @c cmd_ctx.reply impl).
 *
 * @param[in] ctx      The @ref cmd_ctx.
 * @param[in] html     HTML text.
 * @param[in] reply_to Message id to reply to (0 for none).
 *
 * @return 0 if queued, negative errno on failure.
 */
static int router_reply(struct cmd_ctx *ctx, const char *html, int64_t reply_to)
{
    router_reply_ctx_t *rc = ctx->reply_ctx;
    char body[8192];
    int n = snprintf(body, sizeof(body), "{\"chat_id\":%lld,\"text\":\"", (long long)ctx->chat_id);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return -ENOSPC;
    }
    size_t off = json_append(body, sizeof(body), (size_t)n, html);
    if (off == (size_t)-1) {
        return -ENOSPC;
    }

    if (reply_to != 0) {
        n = snprintf(body + off, sizeof(body) - off,
                     "\",\"parse_mode\":\"HTML\",\"reply_to_message_id\":%lld}",
                     (long long)reply_to);
    } else {
        n = snprintf(body + off, sizeof(body) - off, "\",\"parse_mode\":\"HTML\"}");
    }
    if (n < 0 || (size_t)n >= sizeof(body) - off) {
        return -ENOSPC;
    }
    off += (size_t)n;

    return rc->api->request(rc->api->ctx, METHOD_SEND_MESSAGE, body, off, ctx->chat_id, 0,
                            on_ignore_result, NULL);
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
    if (msg->chat.id != 0) {
        db_job_t job = {.kind = DB_JOB_UPSERT_CHAT, .id = msg->chat.id};
        (void)snprintf(job.title, sizeof(job.title), "%s", msg->chat.title);
        (void)snprintf(job.chat_type, sizeof(job.chat_type), "%s", msg->chat.type);
        (void)r->cfg.db->submit(r->cfg.db->ctx, &job, on_upsert, NULL);
    }
}

/* --- Dispatch -------------------------------------------------------------- */

/** @brief Context for the permission-gated dispatch callback. */
typedef struct {
    router_t *router;         /**< Router. */
    tg_message_t message;     /**< Copy of the originating message (async-safe). */
    cmd_invocation_t inv;     /**< Parsed invocation. */
    int64_t target_id;        /**< Resolved target (0 if none). */
    router_reply_ctx_t reply; /**< Reply context. */
} dispatch_t;

/**
 * @brief Send a plain HTML reply to the command's chat.
 *
 * @param[in] d    Dispatch context.
 * @param[in] html HTML text.
 */
static void reply_text(dispatch_t *d, const char *html)
{
    struct cmd_ctx ctx = {
        .chat_id = d->message.chat.id, .reply = router_reply, .reply_ctx = &d->reply};
    (void)ctx.reply(&ctx, html, d->message.message_id);
}

/**
 * @brief Send a permission-denial reply for @p rc.
 *
 * @param[in] d  Dispatch context (for the chat id and reply hook).
 * @param[in] rc The permission error (-ERR_TG_API or -ERR_PERM).
 */
static void reply_denied(dispatch_t *d, int rc)
{
    reply_text(d, rc == -ERR_TG_API ? "I lack the rights to do that in this chat."
                                    : "You are not allowed to use that command.");
}

/**
 * @brief Build the handler context and invoke the command handler.
 *
 * @param[in] d       Dispatch context.
 * @param[in] subject Caller's resolved subject.
 * @param[in] cmd     Command to run.
 */
static void run_handler(dispatch_t *d, const perms_subject_t *subject, const cmd_desc_t *cmd)
{
    const tg_message_t *msg = &d->message;
    arena_t arena;
    if (arena_init(&arena, 1024) != 0) {
        return;
    }
    struct cmd_ctx cctx = {
        .cmd = cmd,
        .chat_id = msg->chat.id,
        .user_id = msg->from.id,
        .target_id = d->target_id,
        .args = d->inv.args,
        .msg = msg,
        .is_group =
            strcmp(msg->chat.type, "group") == 0 || strcmp(msg->chat.type, "supergroup") == 0,
        .role = subject->role,
        .is_sudo = subject->is_sudo,
        .reply = router_reply,
        .reply_ctx = &d->reply,
    };
    int hrc = cmd->handler(&cctx);
    if (hrc != 0) {
        util_log(LOG_WARN, "router", "command '%s' returned %d", cmd->name, hrc);
    }
    arena_destroy(&arena);
}

/**
 * @brief Build a subject without consulting the admin list (member + sudo).
 *
 * Used for private chats, where `getChatAdministrators` is not available at all.
 *
 * @param[in]  r       Router.
 * @param[in]  user_id Caller's user id.
 * @param[out] out     Receives the subject.
 */
static void subject_local(const router_t *r, int64_t user_id, perms_subject_t *out)
{
    memset(out, 0, sizeof(*out));
    out->role = ROLE_ANYONE; /* a "member"; not an administrator */
    out->is_sudo = r->cfg.perms != NULL && perms_is_sudo(r->cfg.perms, user_id);
}

/**
 * @brief Gate on permissions and run the command handler.
 *
 * @param[in] d       Dispatch context.
 * @param[in] subject Caller's resolved subject.
 */
static void dispatch_subject(dispatch_t *d, const perms_subject_t *subject)
{
    const cmd_desc_t *cmd = cmd_lookup(d->inv.name);
    if (cmd == NULL) {
        return; /* unknown command: ignore silently (another bot's, or a typo) */
    }

    /* Group-only commands are rejected in private chats (SPEC section 5.2). */
    int is_group = strcmp(d->message.chat.type, "group") == 0 ||
                   strcmp(d->message.chat.type, "supergroup") == 0;
    if (!is_group && (cmd->flags & CMDF_GROUP_ONLY) != 0u) {
        reply_text(d, "That command is only available in groups.");
        return;
    }

    /* The bot must hold the command's right; a per-right check of the BOT needs
     * its own ChatMember, which arrives with the moderation commands (M4). Until
     * then treat the bot as holding its rights when the fetch succeeded. */
    int rc = perms_check(subject, cmd, 1);
    if (rc != 0) {
        reply_denied(d, rc);
        return;
    }
    run_handler(d, subject, cmd);
}

/**
 * @brief Subject-resolution callback: gate on permissions, then run the handler.
 *
 * @param[in] subject Caller's subject (NULL on resolution failure).
 * @param[in] status  Resolution status.
 * @param[in] ctx     The @ref dispatch_t.
 */
static void on_subject(const perms_subject_t *subject, int status, void *ctx)
{
    dispatch_t *d = ctx;
    if (status != 0 || subject == NULL) {
        /* The admin fetch failed (e.g. a transient Telegram error). Reply rather
         * than dropping the command silently, and never misreport it as a
         * permission denial. */
        util_log(LOG_WARN, "router", "permission check failed: %d", status);
        reply_text(d, "I could not verify permissions in this chat; please try again.");
        free(d);
        return;
    }
    dispatch_subject(d, subject);
    free(d);
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

    dispatch_t *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        return -ENOMEM;
    }
    d->router = r;
    /* Copy the message: the admin fetch is asynchronous, so the caller's stack
     * tg_update_t will be gone by the time the callback runs. */
    d->message = *msg;
    d->reply.api = r->cfg.api;

    int is_cmd = router_parse_command(msg->text, r->cfg.bot_username, &d->inv);
    if (is_cmd <= 0 || d->inv.addressed_to_other_bot) {
        free(d);
        return 0;
    }

    (void)router_resolve_target(&d->inv, msg, &d->target_id);

    int is_group =
        strcmp(msg->chat.type, "group") == 0 || strcmp(msg->chat.type, "supergroup") == 0;

    /* A private chat has no administrator list (getChatAdministrators is rejected
     * there), so resolve the subject locally: the caller is a plain member unless
     * configured as sudo, and role-sensitive output like /help is rendered for
     * that role. Group chats resolve the real role from the (cached) admin list so
     * administrators see the full command set. */
    if (r->cfg.perms == NULL || !is_group) {
        perms_subject_t subject;
        subject_local(r, msg->from.id, &subject);
        dispatch_subject(d, &subject);
        free(d);
        return 0;
    }

    int rc = perms_subject_get(r->cfg.perms, msg->chat.id, msg->from.id, on_subject, d);
    if (rc != 0) {
        free(d);
        return rc;
    }
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
