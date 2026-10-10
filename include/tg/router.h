/**
 * @file router.h
 * @brief Update -> command dispatch: parse, resolve target, check perms, run.
 *
 * The router is the loop-thread entry point for every parsed update. For a message
 * it:
 *  1. upserts the sender and chat through the @ref db_ops_t seam;
 *  2. parses a leading `/command[@bot]`, ignoring commands addressed to another bot;
 *  3. resolves the target user (reply-to, then numeric id) and the trailing reason;
 *  4. resolves the caller's role via @ref perms, checks the command's rights;
 *  5. dispatches to the command handler from @c cmd_table.
 *
 * The pure helpers @ref router_parse_command and @ref router_resolve_target are
 * exposed so the parsing rules are unit-tested without a loop or network.
 *
 * @ingroup router
 */
#ifndef TG_ROUTER_H
#define TG_ROUTER_H

#include <stddef.h>
#include <stdint.h>

#include "tg/db.h"
#include "tg/perms.h"
#include "tg/settings.h"
#include "tg/tg_api.h"
#include "tg/tg_types.h"
#include "tg/util.h"

/** @brief Forward declaration: the automod service (see tg/automod.h). */
typedef struct automod automod_t;
/** @brief Forward declaration: the captcha service (see tg/captcha.h). */
typedef struct captcha captcha_t;

/** @brief Maximum length of a command name (matches the S2 limit). */
#define ROUTER_CMD_NAME_MAX 32
/** @brief Maximum length of a raw target token (`\@username` or numeric id). */
#define ROUTER_TARGET_MAX 64

/** @brief A parsed `/command[@bot] args` invocation. */
typedef struct {
    char name[ROUTER_CMD_NAME_MAX + 1]; /**< Lowercased command name (no '/', no `\@bot`). */
    int addressed_to_other_bot;         /**< 1 if the command was `/cmd\@OtherBot`. */
    int has_target;                     /**< 1 if a leading target token was recognized. */
    char target[ROUTER_TARGET_MAX];     /**< Raw target token (`\@name` or numeric). */
    const char *args;                   /**< Remaining text (points into @p text). */
} cmd_invocation_t;

/** @brief Router configuration. All pointers must outlive the router. */
typedef struct {
    const tg_api_ops_t *api;      /**< API seam (replies + admin fetch). */
    const db_ops_t *db;           /**< DB seam (user/chat upserts, target lookup). */
    perms_t *perms;               /**< Permission engine. */
    settings_t *settings;         /**< Per-chat settings service (may be NULL). */
    automod_t *automod;           /**< Automod service (may be NULL). */
    captcha_t *captcha;           /**< Captcha service (may be NULL). */
    const clock_ops_t *clock;     /**< Injected monotonic clock (unused for now). */
    const wall_clock_ops_t *wall; /**< Injected wall clock (expiry/until_date). */
    int64_t bot_id;               /**< The bot's own user id. */
    const char *bot_username;     /**< The bot's username (no `\@`); may be NULL. */
} router_config_t;

/** @brief The router (opaque; see router.c). */
typedef struct router router_t;

/**
 * @brief Parse a message text into a command invocation.
 *
 * Recognizes a leading `/name` or `/name@bot`, lowercases the name, and treats the
 * text after the first space as arguments. A leading `\@username` or a run of digits
 * in the arguments is captured as the target.
 *
 * @param[in]  text         Message text (must not be NULL).
 * @param[in]  bot_username The bot's own username (no '@'), or NULL to accept any.
 * @param[out] out          Receives the parsed invocation.
 *
 * @return 1 if @p text is a command, 0 if it is not (plain text), -EINVAL on bad
 *         arguments.
 * @ownership @p out->args points into @p text; it is valid as long as @p text is.
 * @thread_safety Safe (no shared state).
 */
int router_parse_command(const char *text, const char *bot_username, cmd_invocation_t *out);

/**
 * @brief Parse a message text into a command invocation, optionally taking a target.
 *
 * Like @ref router_parse_command but @p want_target controls whether a leading
 * `\@username` or numeric token is consumed into @c out->target. Commands that
 * take no target (e.g. `/setwarnlimit 5`) pass 0 so a numeric argument is left in
 * @c out->args rather than being mistaken for a target.
 *
 * @param[in]  text         Message text (must not be NULL).
 * @param[in]  bot_username The bot's own username (no '@'), or NULL to accept any.
 * @param[in]  want_target  1 to capture a leading target token, 0 to leave the
 *                          whole tail in @c out->args.
 * @param[out] out          Receives the parsed invocation.
 *
 * @return 1 if @p text is a command, 0 if it is not (plain text), -EINVAL on bad
 *         arguments.
 * @ownership @p out->args points into @p text; it is valid as long as @p text is.
 * @thread_safety Safe (no shared state).
 */
int router_parse_command_ex(const char *text, const char *bot_username, int want_target,
                            cmd_invocation_t *out);

/**
 * @brief Resolve the target user for @p inv from @p msg.
 *
 * Resolution order (SPEC section 5.2): the replied-to message's sender, then a
 * numeric user id in the target token. `\@username` resolution needs a users-table
 * lookup, which the router performs asynchronously; this helper reports -ENOTSUP
 * for an `\@username` so the caller can start that lookup.
 *
 * @param[in]  inv Parsed invocation.
 * @param[in]  msg Originating message.
 * @param[out] out Receives the resolved user id (0 if none was resolved).
 *
 * @return 0 if a target was resolved, 1 if there was no target, -ENOTSUP for an
 *         unresolvable `\@username` (caller resolves it), -EINVAL on bad arguments.
 * @thread_safety Safe (no shared state).
 */
int router_resolve_target(const cmd_invocation_t *inv, const tg_message_t *msg, int64_t *out);

/**
 * @brief Create the router.
 *
 * @param[in] cfg Configuration (copied; pointers must outlive the router).
 *
 * @return A new router, or NULL on failure.
 * @ownership Caller must call router_destroy().
 * @thread_safety Loop thread only.
 */
router_t *router_create(const router_config_t *cfg);

/**
 * @brief Handle one update (upsert, parse, permission-check, dispatch).
 *
 * @param[in] r   Router.
 * @param[in] upd Parsed update.
 *
 * @return 0 if handled (including ignored updates), negative errno on a fatal
 *         argument error.
 * @thread_safety Loop thread only.
 */
int router_handle(router_t *r, const tg_update_t *upd);

/**
 * @brief Destroy the router.
 *
 * @param[in] r Router (may be NULL).
 * @thread_safety Loop thread only.
 */
void router_destroy(router_t *r);

/**
 * @brief Deliver a join welcome (the callback the captcha service invokes).
 *
 * Wired into @ref captcha_config_t::on_welcome by the runtime so the captcha
 * module never needs the welcome template: after a member passes the challenge
 * (or immediately, when the captcha is disabled) the router fetches the chat
 * policy and sends the rendered welcome (SPEC section 5.4).
 *
 * @param[in] ctx  The @ref router_t.
 * @param[in] chat Chat joined.
 * @param[in] user Joining user.
 * @thread_safety Loop thread only.
 */
void router_on_captcha_welcome(void *ctx, const tg_chat_t *chat, const tg_user_t *user);

#endif /* TG_ROUTER_H */
