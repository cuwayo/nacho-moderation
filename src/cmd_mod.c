/**
 * @file cmd_mod.c
 * @brief Member-moderation handlers: /ban /unban /kick /mute /unmute (SPEC 5.2).
 *
 * Each handler builds the Bot API JSON body, issues the call through
 * @ref cmd_ctx_request (which keeps the invocation alive until the callback runs),
 * records an `actions` row, and replies. The handlers never block and never touch
 * the network directly (SPEC section 9).
 *
 * `/kick` is a two-step flow: `banChatMember` followed by `unbanChatMember` in its
 * completion callback. The invocation is reference-counted, so the unban callback
 * safely runs after the handler has returned (SPEC section 9).
 *
 * @ingroup commands
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_methods.h"
#include "commands.h"
#include "tg/api_pool.h"
#include "tg/cmd_ctx.h"
#include "tg/cmd_util.h"
#include "tg/util.h"

/** @brief Maximum length of a rendered target label / reason used in replies. */
#define CMD_REASON_MAX 256u

/**
 * @brief Guard a moderation command: require a target and that it may be acted on.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command may proceed, 1 if a reply was sent and the handler must
 *         stop, negative errno on a fatal error.
 */
static int guard_target(struct cmd_ctx *ctx)
{
    if (ctx->target_id == 0) {
        (void)cmd_reply_no_target(ctx);
        return 1;
    }
    if (cmd_target_guard(ctx) != 0) {
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "I cannot act on the owner, an administrator, or myself.");
        return 1;
    }
    return 0;
}

/**
 * @brief Render a `[target]` label for replies, HTML-safe.
 *
 * @param[in]  ctx   Context.
 * @param[out] out   Output buffer (HTML-escaped label).
 * @param[in]  cap   Capacity of @p out.
 */
static void render_target(const struct cmd_ctx *ctx, char *out, size_t cap)
{
    const char *label =
        ctx->target_label != NULL && ctx->target_label[0] != '\0' ? ctx->target_label : "the user";
    size_t n = 0;
    (void)util_html_escape(label, strlen(label), out, cap, &n);
}

/* --- /ban -------------------------------------------------------------- */

/** @brief Completion context for a moderation reply (target + action word). */
typedef struct {
    cmd_call_t *call; /**< Invocation kept alive across the call. */
    char verb[24];    /**< Past-tense action ("banned", "muted", ...). */
} mod_reply_ctx_t;

/**
 * @brief Report the outcome of a moderation API call.
 *
 * @param[in] res API result.
 * @param[in] ctx The @ref mod_reply_ctx_t.
 */
static void on_moderation(const api_result_t *res, void *ctx)
{
    mod_reply_ctx_t *m = ctx;
    struct cmd_ctx *c = cmd_call_ctx(m->call);
    int64_t reply_to = c->msg != NULL ? c->msg->message_id : 0;
    if (res->status == 0) {
        char label[CMD_TARGET_LABEL_MAX * 6u + 1u];
        render_target(c, label, sizeof(label));
        (void)cmd_ctx_replyf(c, reply_to, "%s %s.", label, m->verb);
    } else {
        (void)cmd_ctx_replyf(c, reply_to, "That failed: %s",
                             res->description[0] != '\0' ? res->description : "request rejected");
    }
    free(m);
}

/**
 * @brief Allocate a reply context bound to @p ctx with the given verb.
 *
 * @param[in] ctx  Context.
 * @param[in] verb Past-tense verb for a success reply.
 *
 * @return The context, or NULL on allocation failure.
 */
static mod_reply_ctx_t *mod_reply_new(struct cmd_ctx *ctx, const char *verb)
{
    mod_reply_ctx_t *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        return NULL;
    }
    m->call = ctx->call;
    (void)snprintf(m->verb, sizeof(m->verb), "%s", verb);
    return m;
}

/**
 * @brief Issue a moderation call that reports its outcome via @c on_moderation.
 *
 * @param[in] ctx    Context.
 * @param[in] method API method.
 * @param[in] body   JSON body.
 * @param[in] verb   Past-tense verb for a success reply.
 *
 * @return 0 if queued, negative errno on failure.
 */
static int issue_moderation(struct cmd_ctx *ctx, method_id_t method, const char *body,
                            const char *verb)
{
    mod_reply_ctx_t *m = mod_reply_new(ctx, verb);
    if (m == NULL) {
        return -ENOMEM;
    }
    int rc = cmd_ctx_request(ctx, method, body, strlen(body), on_moderation, m);
    if (rc != 0) {
        free(m);
    }
    return rc;
}

/**
 * @brief /ban handler: ban the target, optionally for a duration.
 *
 * @param[in] ctx Context (target resolved by the router).
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_ban(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    int g = guard_target(ctx);
    if (g != 0) {
        return g > 0 ? 0 : g;
    }
    int64_t duration = 0;
    const char *reason = "";
    (void)cmd_parse_duration_reason(ctx->args, &duration, &reason);
    int64_t until = duration > 0 ? cmd_ctx_now(ctx) + duration : 0;

    char body[256];
    if (until > 0) {
        (void)snprintf(body, sizeof(body),
                       "{\"chat_id\":%lld,\"user_id\":%lld,\"until_date\":%lld,"
                       "\"revoke_messages\":true}",
                       (long long)ctx->chat_id, (long long)ctx->target_id, (long long)until);
    } else {
        (void)snprintf(body, sizeof(body),
                       "{\"chat_id\":%lld,\"user_id\":%lld,\"revoke_messages\":true}",
                       (long long)ctx->chat_id, (long long)ctx->target_id);
    }

    if (ctx->record_action != NULL) {
        ctx->record_action(ctx, "ban", ctx->target_id, reason, until);
    }
    return issue_moderation(ctx, METHOD_BAN_CHAT_MEMBER, body, "banned");
}

/**
 * @brief /unban handler: lift the target's ban (`only_if_banned`).
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_unban(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    if (ctx->target_id == 0) {
        (void)cmd_reply_no_target(ctx);
        return 0;
    }
    char body[256];
    (void)snprintf(body, sizeof(body),
                   "{\"chat_id\":%lld,\"user_id\":%lld,\"only_if_banned\":true}",
                   (long long)ctx->chat_id, (long long)ctx->target_id);
    return issue_moderation(ctx, METHOD_UNBAN_CHAT_MEMBER, body, "unbanned");
}

/** @brief Context for the /kick ban -> unban continuation. */
typedef struct {
    cmd_call_t *call; /**< Invocation kept alive across both calls. */
} kick_ctx_t;

/**
 * @brief /kick step 2: unban the user so they may rejoin (SPEC section 5.2).
 *
 * @param[in] res Ban result.
 * @param[in] ctx The @ref kick_ctx_t.
 */
static void on_kick_unban(const api_result_t *res, void *ctx)
{
    kick_ctx_t *k = ctx;
    struct cmd_ctx *c = cmd_call_ctx(k->call);
    int64_t reply_to = c->msg != NULL ? c->msg->message_id : 0;
    if (res->status == 0) {
        char label[CMD_TARGET_LABEL_MAX * 6u + 1u];
        render_target(c, label, sizeof(label));
        (void)cmd_ctx_replyf(c, reply_to, "%s was kicked.", label);
    } else {
        (void)cmd_ctx_replyf(c, reply_to, "That failed: %s",
                             res->description[0] != '\0' ? res->description : "request rejected");
    }
    free(k);
}

/**
 * @brief /kick step 1 completion: on success, immediately unban.
 *
 * @param[in] res Ban result.
 * @param[in] ctx The @ref kick_ctx_t.
 */
static void on_kick_ban(const api_result_t *res, void *ctx)
{
    kick_ctx_t *k = ctx;
    struct cmd_ctx *c = cmd_call_ctx(k->call);
    if (res->status != 0) {
        int64_t reply_to = c->msg != NULL ? c->msg->message_id : 0;
        (void)cmd_ctx_replyf(c, reply_to, "That failed: %s",
                             res->description[0] != '\0' ? res->description : "request rejected");
        free(k);
        return;
    }
    char body[256];
    (void)snprintf(body, sizeof(body),
                   "{\"chat_id\":%lld,\"user_id\":%lld,\"only_if_banned\":true}",
                   (long long)c->chat_id, (long long)c->target_id);
    int rc = cmd_ctx_request(c, METHOD_UNBAN_CHAT_MEMBER, body, strlen(body), on_kick_unban, k);
    if (rc != 0) {
        int64_t reply_to = c->msg != NULL ? c->msg->message_id : 0;
        (void)cmd_ctx_replyf(c, reply_to, "That failed: could not complete the kick.");
        free(k);
    }
}

/**
 * @brief /kick handler: ban the target then immediately unban them.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_kick(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    int g = guard_target(ctx);
    if (g != 0) {
        return g > 0 ? 0 : g;
    }
    const char *reason = "";
    int64_t duration = 0;
    (void)cmd_parse_duration_reason(ctx->args, &duration, &reason);

    kick_ctx_t *k = calloc(1, sizeof(*k));
    if (k == NULL) {
        return -ENOMEM;
    }
    k->call = ctx->call;
    char body[256];
    (void)snprintf(body, sizeof(body),
                   "{\"chat_id\":%lld,\"user_id\":%lld,\"revoke_messages\":true}",
                   (long long)ctx->chat_id, (long long)ctx->target_id);
    if (ctx->record_action != NULL) {
        ctx->record_action(ctx, "kick", ctx->target_id, reason, 0);
    }
    int rc = cmd_ctx_request(ctx, METHOD_BAN_CHAT_MEMBER, body, strlen(body), on_kick_ban, k);
    if (rc != 0) {
        free(k);
    }
    return rc;
}

/**
 * @brief /mute handler: clear the target's send permissions, optionally for a
 *        duration.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_mute(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    int g = guard_target(ctx);
    if (g != 0) {
        return g > 0 ? 0 : g;
    }
    int64_t duration = 0;
    const char *reason = "";
    (void)cmd_parse_duration_reason(ctx->args, &duration, &reason);
    int64_t until = duration > 0 ? cmd_ctx_now(ctx) + duration : 0;

    /* All permissions false: the user may only read (SPEC section 5.2). */
    static const char *const perms_false =
        "\"can_send_messages\":false,\"can_send_audios\":false,\"can_send_documents\":false,"
        "\"can_send_photos\":false,\"can_send_videos\":false,\"can_send_video_notes\":false,"
        "\"can_send_voice_notes\":false,\"can_send_polls\":false,\"can_send_other_messages\":false,"
        "\"can_add_web_page_previews\":false,\"can_change_info\":false,\"can_invite_users\":false,"
        "\"can_pin_messages\":false";
    char body[1024];
    if (until > 0) {
        (void)snprintf(body, sizeof(body),
                       "{\"chat_id\":%lld,\"user_id\":%lld,\"until_date\":%lld,"
                       "\"permissions\":{%s}}",
                       (long long)ctx->chat_id, (long long)ctx->target_id, (long long)until,
                       perms_false);
    } else {
        (void)snprintf(body, sizeof(body),
                       "{\"chat_id\":%lld,\"user_id\":%lld,\"permissions\":{%s}}",
                       (long long)ctx->chat_id, (long long)ctx->target_id, perms_false);
    }

    if (ctx->record_action != NULL) {
        ctx->record_action(ctx, "mute", ctx->target_id, reason, until);
    }
    return issue_moderation(ctx, METHOD_RESTRICT_CHAT_MEMBER, body, "muted");
}

/**
 * @brief /unmute handler: restore the target's common group permissions.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_unmute(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    if (ctx->target_id == 0) {
        (void)cmd_reply_no_target(ctx);
        return 0;
    }
    /* Grant the common group permissions back. */
    static const char *const perms_true =
        "\"can_send_messages\":true,\"can_send_audios\":true,\"can_send_documents\":true,"
        "\"can_send_photos\":true,\"can_send_videos\":true,\"can_send_video_notes\":true,"
        "\"can_send_voice_notes\":true,\"can_send_polls\":true,\"can_send_other_messages\":true,"
        "\"can_add_web_page_previews\":true,\"can_invite_users\":true";
    char body[1024];
    (void)snprintf(body, sizeof(body), "{\"chat_id\":%lld,\"user_id\":%lld,\"permissions\":{%s}}",
                   (long long)ctx->chat_id, (long long)ctx->target_id, perms_true);
    return issue_moderation(ctx, METHOD_RESTRICT_CHAT_MEMBER, body, "unmuted");
}
