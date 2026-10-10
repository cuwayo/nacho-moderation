/**
 * @file cmd_warn.c
 * @brief Warning handlers: /warn /unwarn /warns /resetwarns (SPEC section 5.2).
 *
 * `/warn` records a warning in the `warns` table and, when the per-chat limit is
 * reached, applies the configured action (ban, mute or kick) and clears the
 * warnings (SPEC section 5.2). The DB work runs through @ref cmd_ctx::db_submit,
 * which keeps the invocation alive until the job completes; the auto-action
 * reuses the existing moderation handlers so its behaviour matches `/ban`,
 * `/mute` and `/kick` exactly.
 *
 * @ingroup commands
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "tables.h"
#include "tg/cmd_ctx.h"
#include "tg/cmd_util.h"
#include "tg/db.h"
#include "tg/util.h"

/** @brief Default auto-action when a chat has not chosen one (SPEC section 6). */
#define WARN_DEFAULT_ACTION "mute"

/**
 * @brief Guard a warning command: require a target that may be acted on.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command may proceed, 1 if a reply was sent, negative errno on
 *         a fatal error.
 */
static int warn_guard_target(struct cmd_ctx *ctx)
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

/* --- /warn ----------------------------------------------------------------- */

/** @brief Context for the /warn add -> (optional) auto-action chain. */
typedef struct {
    cmd_call_t *call; /**< Invocation kept alive across the DB job. */
    char reason[256]; /**< Reason carried into the auto-action. */
} warn_ctx_t;

/**
 * @brief Apply the chat's configured warn action by reusing the moderation handler.
 *
 * Delegates to `/ban`, `/mute` or `/kick` with the reason in @c ctx->args, so the
 * action's API call, target guard and `actions` row are identical to a manual run.
 *
 * @param[in] ctx    Context (target resolved, @c args holds the reason).
 * @param[in] action Action name ("ban"/"mute"/"kick").
 *
 * @return 0 if queued, negative errno on failure; 1 if the action was unknown.
 */
static int warn_apply_action(struct cmd_ctx *ctx, const char *action)
{
    if (strcmp(action, "ban") == 0) {
        return cmd_ban(ctx);
    }
    if (strcmp(action, "mute") == 0) {
        return cmd_mute(ctx);
    }
    if (strcmp(action, "kick") == 0) {
        return cmd_kick(ctx);
    }
    return 1; /* unknown action: leave the warnings in place */
}

/**
 * @brief Apply the configured auto-action and clear the warnings.
 *
 * @param[in] ctx    Context.
 * @param[in] reason Reason to carry into the action.
 * @param[in] action Action name ("ban"/"mute"/"kick").
 */
static void warn_auto_act(struct cmd_ctx *ctx, const char *reason, const char *action)
{
    ctx->args = reason; /* the moderation handler reads the reason from args */
    if (warn_apply_action(ctx, action) != 0) {
        return; /* unknown action: leave the warnings in place */
    }
    /* Clear the warnings so the next cycle starts fresh. */
    if (ctx->db_submit != NULL) {
        db_job_t reset = {.kind = DB_JOB_WARN_RESET, .id = ctx->chat_id, .user_id = ctx->target_id};
        (void)ctx->db_submit(ctx, &reset, NULL, NULL);
    }
}

/**
 * @brief /warn add completion: report the count and auto-act at the limit.
 *
 * @param[in] res DB result (its @c i64 is the new warn count).
 * @param[in] user The @ref warn_ctx_t.
 */
static void on_warn_added(const db_result_t *res, void *user)
{
    warn_ctx_t *w = user;
    struct cmd_ctx *ctx = cmd_call_ctx(w->call);
    int64_t reply_to = ctx->msg != NULL ? ctx->msg->message_id : 0;
    if (res->status != 0) {
        (void)cmd_ctx_reply_plain(ctx, reply_to, "I could not record that warning.");
        free(w);
        return;
    }
    int64_t count = res->i64;
    const chat_settings_t *s = ctx->settings;
    int limit = s != NULL ? s->warn_limit : 0;
    const char *action =
        (s != NULL && s->warn_action[0] != '\0') ? s->warn_action : WARN_DEFAULT_ACTION;
    const char *label = ctx->target_label != NULL ? ctx->target_label : "the user";

    if (limit > 0 && count >= limit) {
        (void)cmd_ctx_replyf(ctx, reply_to, "Warning limit (%d) reached; applying '%s'.", limit,
                             action);
        warn_auto_act(ctx, w->reason, action);
    } else if (limit > 0) {
        (void)cmd_ctx_replyf(ctx, reply_to, "Warned %s (%lld/%d).", label, (long long)count, limit);
    } else {
        (void)cmd_ctx_replyf(ctx, reply_to, "Warned %s (%lld).", label, (long long)count);
    }
    free(w);
}

/**
 * @brief /warn handler: add a warning and auto-act at the limit.
 *
 * @param[in] ctx Context (target resolved by the router).
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_warn(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    int g = warn_guard_target(ctx);
    if (g != 0) {
        return g > 0 ? 0 : g;
    }
    if (ctx->db_submit == NULL) {
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "Warnings are unavailable right now.");
        return 0;
    }

    warn_ctx_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return -ENOMEM;
    }
    w->call = ctx->call;
    (void)snprintf(w->reason, sizeof(w->reason), "%s", ctx->args != NULL ? ctx->args : "");

    db_job_t job = {.kind = DB_JOB_WARN_ADD,
                    .id = ctx->chat_id,
                    .user_id = ctx->target_id,
                    .admin_id = ctx->user_id,
                    .created_at = cmd_ctx_now(ctx)};
    (void)snprintf(job.reason, sizeof(job.reason), "%s", w->reason);
    int rc = ctx->db_submit(ctx, &job, on_warn_added, w);
    if (rc != 0) {
        free(w);
    }
    return rc;
}

/* --- /unwarn /resetwarns --------------------------------------------------- */

/** @brief Completion for a fire-and-forget warn mutation that replies on success. */
typedef struct {
    cmd_call_t *call; /**< Invocation kept alive across the DB job. */
    char verb[64];    /**< Reply text on success (already plain). */
} warn_mut_ctx_t;

/**
 * @brief Report the outcome of a warn mutation (unwarn/resetwarns).
 *
 * @param[in] res DB result.
 * @param[in] user The @ref warn_mut_ctx_t.
 */
static void on_warn_mutated(const db_result_t *res, void *user)
{
    warn_mut_ctx_t *m = user;
    struct cmd_ctx *ctx = cmd_call_ctx(m->call);
    int64_t reply_to = ctx->msg != NULL ? ctx->msg->message_id : 0;
    if (res->status == 0) {
        (void)cmd_ctx_reply_plain(ctx, reply_to, m->verb);
    } else {
        (void)cmd_ctx_reply_plain(ctx, reply_to, "That failed.");
    }
    free(m);
}

/**
 * @brief Submit a warn mutation that replies with @p verb on success.
 *
 * @param[in] ctx  Context.
 * @param[in] kind Job kind (remove-last or reset).
 * @param[in] verb Success reply text.
 *
 * @return 0 if queued, negative errno on failure.
 */
static int warn_mutate(struct cmd_ctx *ctx, db_job_kind_t kind, const char *verb)
{
    if (ctx->db_submit == NULL) {
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "Warnings are unavailable right now.");
        return 0;
    }
    warn_mut_ctx_t *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        return -ENOMEM;
    }
    m->call = ctx->call;
    (void)snprintf(m->verb, sizeof(m->verb), "%s", verb);
    db_job_t job = {.kind = kind, .id = ctx->chat_id, .user_id = ctx->target_id};
    int rc = ctx->db_submit(ctx, &job, on_warn_mutated, m);
    if (rc != 0) {
        free(m);
    }
    return rc;
}

/**
 * @brief /unwarn handler: remove the target's newest warning.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_unwarn(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    if (ctx->target_id == 0) {
        (void)cmd_reply_no_target(ctx);
        return 0;
    }
    return warn_mutate(ctx, DB_JOB_WARN_REMOVE_LAST, "Removed the newest warning.");
}

/**
 * @brief /resetwarns handler: clear every warning for the target.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_resetwarns(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    if (ctx->target_id == 0) {
        (void)cmd_reply_no_target(ctx);
        return 0;
    }
    return warn_mutate(ctx, DB_JOB_WARN_RESET, "Cleared all warnings.");
}

/* --- /warns ---------------------------------------------------------------- */

/** @brief Context for the warn-list query. */
typedef struct {
    cmd_call_t *call; /**< Invocation kept alive across the DB job. */
    int self;         /**< 1 if the caller is listing their own warnings. */
} warn_list_ctx_t;

/**
 * @brief Warn-list completion: render the warnings and reply.
 *
 * @param[in] res DB result carrying @c warns / @c warn_count.
 * @param[in] user The @ref warn_list_ctx_t.
 */
static void on_warns_listed(const db_result_t *res, void *user)
{
    warn_list_ctx_t *l = user;
    struct cmd_ctx *ctx = cmd_call_ctx(l->call);
    int64_t reply_to = ctx->msg != NULL ? ctx->msg->message_id : 0;
    if (res->status != 0) {
        (void)cmd_ctx_reply_plain(ctx, reply_to, "I could not read the warnings.");
        free(l);
        return;
    }
    const char *label = ctx->target_label != NULL ? ctx->target_label : "the user";
    strbuf_t out;
    if (strbuf_init(&out, 256) != 0) {
        free(l);
        return;
    }
    if (l->self != 0) {
        (void)strbuf_appendf(&out, "You have %u warning(s):\n", res->warn_count);
    } else {
        (void)strbuf_appendf(&out, "%s has %u warning(s):\n", label, res->warn_count);
    }
    for (unsigned i = 0; i < res->warn_count; ++i) {
        (void)strbuf_appendf(&out, "%u. %s\n", i + 1,
                             res->warns[i].reason[0] != '\0' ? res->warns[i].reason
                                                             : "(no reason)");
    }
    (void)cmd_ctx_reply_plain(ctx, reply_to, strbuf_cstr(&out));
    strbuf_free(&out);
    free(l);
}

/**
 * @brief /warns handler: list the caller's own warnings, or an admin-requested target.
 *
 * With no target the command is self-service: any member sees their own warnings
 * (SPEC section 5.2). Naming a different target is a moderation action and
 * requires an administrator, owner or sudo caller.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_warns(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    int self = ctx->target_id == 0 || ctx->target_id == ctx->user_id;
    if (!self && ctx->role != ROLE_ADMIN && ctx->role != ROLE_OWNER && !ctx->is_sudo) {
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "Only administrators can list another user's warnings.");
        return 0;
    }
    if (ctx->db_submit == NULL) {
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "Warnings are unavailable right now.");
        return 0;
    }
    warn_list_ctx_t *l = calloc(1, sizeof(*l));
    if (l == NULL) {
        return -ENOMEM;
    }
    l->call = ctx->call;
    l->self = self;
    int64_t user_id = self ? ctx->user_id : ctx->target_id;
    db_job_t job = {.kind = DB_JOB_WARN_LIST, .id = ctx->chat_id, .user_id = user_id};
    int rc = ctx->db_submit(ctx, &job, on_warns_listed, l);
    if (rc != 0) {
        free(l);
    }
    return rc;
}
