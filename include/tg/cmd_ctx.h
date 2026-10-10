/**
 * @file cmd_ctx.h
 * @brief The per-invocation command context shared by every handler.
 *
 * commands.h forward-declares @c cmd_ctx_t so the command table can name handler
 * prototypes; this header provides the full definition for the handlers themselves
 * (and for the router that fills it in). Keeping the definition out of commands.h
 * avoids a dependency cycle: the router needs the command table, and handlers need
 * the context.
 *
 * A command can outlive the update that triggered it: `/kick` issues a ban and then
 * an unban from the ban's completion callback, and `/purge` fans out several
 * `deleteMessages` calls (SPEC section 9, "context freed on both success and
 * failure"). The context therefore lives inside a heap-allocated, reference-counted
 * @ref cmd_call_t. The router holds one reference for the duration of the handler
 * call; every API call issued through @ref cmd_ctx_request holds another and drops
 * it when its callback runs. The invocation is freed when the last reference goes
 * away.
 *
 * @ingroup commands
 */
#ifndef TG_CMD_CTX_H
#define TG_CMD_CTX_H

#include <stddef.h>
#include <stdint.h>

#include "api_methods.h"
#include "commands.h"
#include "tables.h"
#include "tg/db.h"
#include "tg/tg_api.h"
#include "tg/tg_types.h"
#include "tg/util.h"
/** @brief A reference-counted command invocation (opaque; see cmd_ctx.c). */
typedef struct cmd_call cmd_call_t;

/** @brief Maximum length of a target's display label (name, \@username or id). */
#define CMD_TARGET_LABEL_MAX 128u

/** @brief Everything a command handler needs for one invocation. */
struct cmd_ctx {
    const cmd_desc_t *cmd;    /**< The command being run (never NULL). */
    int64_t chat_id;          /**< Chat the command was sent in. */
    int64_t user_id;          /**< Caller's user id. */
    int64_t bot_id;           /**< The bot's own user id. */
    int64_t target_id;        /**< Resolved target user id (0 if none). */
    role_t target_role;       /**< Target's role in the chat (for the target guard). */
    const char *target_label; /**< Human-readable target ("Ada", "\@spammer", "123"); never NULL. */
    const char *args;         /**< Text after the command and target (never NULL). */
    const tg_message_t *msg;  /**< The originating message (never NULL). */
    const char *callback_query_id; /**< Callback query id, or NULL for a command. */
    int is_group;                  /**< 1 if the chat is a group/supergroup. */
    role_t role;                   /**< Caller's resolved role in the chat. */
    int is_sudo;                   /**< 1 if the caller is a configured sudo user. */
    int bot_has_right;             /**< 1 if the bot holds @c cmd->bot_right in the chat. */

    const wall_clock_ops_t *wall; /**< Wall clock for `until_date` / `expires_at`. */

    /**
     * @brief The chat's policy snapshot, or NULL when unavailable (private chat).
     *
     * Points into the owning @ref cmd_call and is valid for the whole invocation.
     * Handlers that need the warn limit/action, welcome text or log channel read
     * it here; the router fills it before dispatch (SPEC section 5.2, 5.5).
     */
    const chat_settings_t *settings;

    /**
     * @brief Send a reply to the command's chat.
     *
     * Wraps `sendMessage` with the given HTML text and optional reply-to message
     * id (0 for none). Never blocks. The @p html text is sent verbatim, so callers
     * must HTML-escape user input (prefer @ref cmd_ctx_reply_plain).
     *
     * @param[in] ctx      The context.
     * @param[in] html     HTML-escaped message text.
     * @param[in] reply_to Message id to reply to (0 for none).
     *
     * @return 0 if queued, negative errno on failure.
     */
    int (*reply)(struct cmd_ctx *ctx, const char *html, int64_t reply_to);
    void *reply_ctx; /**< Opaque context for @ref reply (owned by the call). */

    /**
     * @brief Record a moderation action (an `actions` row; SPEC section 5.5).
     *
     * Writes the row through the DB seam, fire and forget, and, when a log
     * channel is configured (@c settings->log_chat_id != 0), posts a moderation
     * log message there (SPEC section 5.5).
     *
     * @param[in] ctx        The context.
     * @param[in] action     Action name ("ban", "mute", ...).
     * @param[in] target_id  User the action applies to.
     * @param[in] reason     Free-text reason (may be "").
     * @param[in] expires_at Unix time the action expires; 0 = permanent.
     */
    void (*record_action)(struct cmd_ctx *ctx, const char *action, int64_t target_id,
                          const char *reason, int64_t expires_at);
    void *record_action_ctx; /**< Opaque context for @ref record_action. */

    /**
     * @brief Submit a DB job that keeps the invocation alive until it completes.
     *
     * Retains the @ref cmd_call for the duration of the job and releases it after
     * @p cb returns, so a handler may start an asynchronous DB chain and return
     * (the same lifetime rule as @ref cmd_ctx_request). Fire and forget.
     *
     * @param[in] ctx  The context.
     * @param[in] job  Job to submit (copied by the seam).
     * @param[in] cb   Completion callback (may be NULL).
     * @param[in] user Opaque pointer passed to @p cb.
     *
     * @return 0 if queued, negative errno on failure (the callback does not run then).
     * @ownership @p user must stay valid until @p cb runs.
     * @thread_safety Loop thread only.
     */
    int (*db_submit)(struct cmd_ctx *ctx, const db_job_t *job, db_cb cb, void *user);
    void *db_submit_ctx; /**< Opaque context for @ref db_submit. */

    /**
     * @brief Drop the cached administrator list for the command's chat.
     *
     * Used by `/reload` (SPEC section 5.1). A no-op when no permission engine is
     * wired.
     *
     * @param[in] ctx The context.
     * @thread_safety Loop thread only.
     */
    void (*invalidate_admins)(struct cmd_ctx *ctx);
    void *invalidate_admins_ctx; /**< Opaque context for @ref invalidate_admins. */

    /**
     * @brief Drop the cached settings for the command's chat.
     *
     * Called by the settings-writing handlers after a successful write so the
     * next read refetches. A no-op when no settings service is wired.
     *
     * @param[in] ctx The context.
     * @thread_safety Loop thread only.
     */
    void (*invalidate_settings)(struct cmd_ctx *ctx);
    void *invalidate_settings_ctx; /**< Opaque context for @ref invalidate_settings. */

    cmd_call_t *call; /**< Owning reference-counted invocation (never NULL). */
};

/**
 * @brief Create a command invocation with one reference held by the caller.
 *
 * @param[in] api  API seam used by @ref cmd_ctx_request (must outlive the call).
 * @param[in] db   DB seam used by the default @c record_action (may be NULL).
 * @param[in] wall Wall clock used for `until_date`/`expires_at`.
 *
 * @return A new invocation, or NULL on allocation failure.
 * @ownership Caller must call @ref cmd_call_release when done.
 * @thread_safety Loop thread only.
 */
cmd_call_t *cmd_call_create(const tg_api_ops_t *api, const db_ops_t *db,
                            const wall_clock_ops_t *wall);

/**
 * @brief Return the context embedded in @p call, to be filled by the router.
 *
 * The @c reply and @c record_action hooks are pre-wired to the call's seams.
 *
 * @param[in] call Invocation.
 *
 * @return The context, or NULL if @p call is NULL.
 * @ownership Owned by @p call.
 * @thread_safety Loop thread only.
 */
struct cmd_ctx *cmd_call_ctx(cmd_call_t *call);

/**
 * @brief Return the message slot embedded in @p call for the router to fill.
 *
 * @param[in] call Invocation.
 *
 * @return The message, or NULL if @p call is NULL.
 * @ownership Owned by @p call.
 * @thread_safety Loop thread only.
 */
tg_message_t *cmd_call_message(cmd_call_t *call);

/**
 * @brief Copy a display label for the resolved target into @p call.
 *
 * The router fills this from the target's known name/username (or the numeric id)
 * so handlers can mention the target in replies without re-parsing the message.
 *
 * @param[in] call  Invocation.
 * @param[in] label Label text (may be NULL to clear it).
 *
 * @return 0 on success, -EINVAL on bad arguments.
 * @thread_safety Loop thread only.
 */
int cmd_call_set_target_label(cmd_call_t *call, const char *label);

/**
 * @brief Wire the admin-cache invalidator used by @c ctx.invalidate_admins.
 *
 * The router owns the permission engine and calls this once per invocation so
 * `/reload` can drop the cached administrator list (SPEC section 5.1).
 *
 * @param[in] call Invocation.
 * @param[in] fn   Invalidator taking the implementation pointer and a chat id
 *                 (may be NULL to disable).
 * @param[in] impl Opaque implementation pointer (the permission engine).
 * @thread_safety Loop thread only.
 */
void cmd_call_set_admin_invalidator(cmd_call_t *call, void (*fn)(void *impl, int64_t chat_id),
                                    void *impl);

/**
 * @brief Wire the settings-cache invalidator used by @c ctx.invalidate_settings.
 *
 * The router owns the settings service and calls this once per invocation so the
 * settings-writing handlers can drop the cached policy after a write.
 *
 * @param[in] call Invocation.
 * @param[in] fn   Invalidator taking the implementation pointer and a chat id
 *                 (may be NULL to disable).
 * @param[in] impl Opaque implementation pointer (the settings service).
 * @thread_safety Loop thread only.
 */
void cmd_call_set_settings_invalidator(cmd_call_t *call, void (*fn)(void *impl, int64_t chat_id),
                                       void *impl);

/**
 * @brief Add one reference to @p call.
 *
 * @param[in] call Invocation (may be NULL; a no-op then).
 * @thread_safety Loop thread only.
 */
void cmd_call_retain(cmd_call_t *call);

/**
 * @brief Drop one reference to @p call, freeing it when the count reaches zero.
 *
 * @param[in] call Invocation (may be NULL; a no-op then).
 * @thread_safety Loop thread only.
 */
void cmd_call_release(cmd_call_t *call);

/**
 * @brief Issue a Bot API call that keeps @p ctx alive until @p cb returns.
 *
 * Retains @p ctx for the duration of the request and releases it after @p cb
 * returns, so a handler may safely issue an asynchronous call and then return.
 *
 * @param[in] ctx    Context.
 * @param[in] method API method identifier.
 * @param[in] body   JSON request body (may be NULL when @p len is 0).
 * @param[in] len    Length of @p body.
 * @param[in] cb     Completion callback (may be NULL for fire-and-forget).
 * @param[in] user   Opaque pointer passed to @p cb.
 *
 * @return 0 if queued, negative errno on failure (the callback does not run then).
 * @ownership @p user must stay valid until @p cb runs; @p ctx is kept alive
 *            automatically.
 * @thread_safety Loop thread only.
 */
int cmd_ctx_request(struct cmd_ctx *ctx, method_id_t method, const char *body, size_t len,
                    tg_api_cb cb, void *user);

/**
 * @brief Like @ref cmd_ctx_request but rate-limited against an explicit chat.
 *
 * Used by the moderation log: the reply/action targets the originating chat, but
 * the log message goes to `settings->log_chat_id`, which must be the bucket the
 * rate limiter accounts for. Retains @p ctx exactly like @ref cmd_ctx_request.
 *
 * @param[in] ctx     Context.
 * @param[in] method  API method identifier.
 * @param[in] body    JSON request body (may be NULL when @p len is 0).
 * @param[in] len     Length of @p body.
 * @param[in] chat_id Chat used for per-chat rate limiting.
 * @param[in] cb      Completion callback (may be NULL for fire-and-forget).
 * @param[in] user    Opaque pointer passed to @p cb.
 *
 * @return 0 if queued, negative errno on failure (the callback does not run then).
 * @ownership @p user must stay valid until @p cb runs; @p ctx is kept alive
 *            automatically.
 * @thread_safety Loop thread only.
 */
int cmd_ctx_request_chat(struct cmd_ctx *ctx, method_id_t method, const char *body, size_t len,
                         int64_t chat_id, tg_api_cb cb, void *user);

/**
 * @brief Copy @p settings into the invocation so @c ctx->settings stays valid.
 *
 * The router calls this after the settings fetch completes; the value is copied
 * (not pointed at the cache) so a later cache eviction cannot dangle the pointer.
 *
 * @param[in] ctx      Context.
 * @param[in] settings Settings to copy (may be NULL to clear).
 *
 * @return 0 on success, -EINVAL on bad arguments.
 * @thread_safety Loop thread only.
 */
int cmd_ctx_set_settings(struct cmd_ctx *ctx, const chat_settings_t *settings);

/**
 * @brief Build a `sendMessage` JSON body carrying @p html as an HTML message.
 *
 * The @p html text is JSON-escaped so markup such as an `{mention}` link can be
 * embedded safely. Exposed for callers that build a `sendMessage` body outside a
 * command invocation (the router's welcome delivery).
 *
 * @param[out] body     Destination buffer.
 * @param[in]  cap      Capacity of @p body.
 * @param[in]  chat_id  Target chat.
 * @param[in]  html     HTML text.
 * @param[in]  reply_to Message id to reply to (0 for none).
 * @param[out] outlen   Receives the body length (excluding NUL).
 *
 * @return 0 on success, -ENOSPC if @p body is too small.
 * @thread_safety Safe (no shared state).
 */
int cmd_ctx_build_send_message(char *body, size_t cap, int64_t chat_id, const char *html,
                               int64_t reply_to, size_t *outlen);

/**
 * @brief Send @p text as an HTML reply, escaping it first (SPEC section 9).
 *
 * @param[in] ctx      Context.
 * @param[in] reply_to Message id to reply to (0 for none).
 * @param[in] text     Plain text (HTML-escaped before sending).
 *
 * @return 0 if queued, negative errno on failure.
 * @thread_safety Loop thread only.
 */
int cmd_ctx_reply_plain(struct cmd_ctx *ctx, int64_t reply_to, const char *text);

/**
 * @brief Format then send an HTML reply, escaping the result (SPEC section 9).
 *
 * @param[in] ctx      Context.
 * @param[in] reply_to Message id to reply to (0 for none).
 * @param[in] fmt      printf format producing plain text.
 * @param[in] ...      Format arguments.
 *
 * @return 0 if queued, negative errno on failure.
 * @thread_safety Loop thread only.
 */
int cmd_ctx_replyf(struct cmd_ctx *ctx, int64_t reply_to, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/**
 * @brief Read the wall clock carried by @p ctx.
 *
 * @param[in] ctx Context.
 *
 * @return Unix seconds (0 if the clock is unavailable).
 * @thread_safety Safe.
 */
int64_t cmd_ctx_now(const struct cmd_ctx *ctx);

#endif /* TG_CMD_CTX_H */
