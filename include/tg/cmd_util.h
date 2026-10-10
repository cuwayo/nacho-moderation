/**
 * @file cmd_util.h
 * @brief Helpers shared by the moderation command handlers (SPEC section 5.2).
 *
 * The M4 commands share a small amount of argument handling: splitting the
 * trailing `[duration] [reason]`, guarding the target against the owner/admins and
 * the bot itself, and escaping a display name for HTML. These are kept here so the
 * individual `cmd_*.c` files stay short and the parsing rules are tested once.
 *
 * @ingroup commands
 */
#ifndef TG_CMD_UTIL_H
#define TG_CMD_UTIL_H

#include <stddef.h>
#include <stdint.h>

#include "tg/cmd_ctx.h"

/**
 * @brief Split the command tail into an optional duration and a reason.
 *
 * The tail is @c ctx->args (everything after the command and its target token).
 * If it starts with a duration literal (`30s`, `10m`, `2h`, `7d`, `1w`, or a bare
 * integer of seconds) that token is parsed with @ref util_parse_duration and
 * removed; the rest, trimmed of leading spaces, is the reason. A tail that does
 * not start with a valid duration is treated entirely as the reason.
 *
 * @param[in]  args         Command tail (may be NULL).
 * @param[out] duration_s   Receives the duration in seconds (0 if none/permanent).
 * @param[out] reason       Receives a pointer into @p args at the reason (never NULL).
 *
 * @return 0 on success (including "no duration"), -EINVAL on bad arguments.
 * @ownership @p *reason points into @p args; valid as long as @p args is.
 * @thread_safety Safe (no shared state).
 */
int cmd_parse_duration_reason(const char *args, int64_t *duration_s, const char **reason);

/**
 * @brief Report whether the command's target may be acted on.
 *
 * Rejects the chat owner, any other administrator, and the bot itself (SPEC
 * section 5.1). Uses @c ctx->target_id, @c ctx->target_role and @c ctx->bot_id.
 *
 * @param[in] ctx Context (target already resolved).
 *
 * @return 0 if the target may be acted on, -ERR_PERM otherwise.
 * @thread_safety Safe (no shared state).
 */
int cmd_target_guard(const struct cmd_ctx *ctx);

/**
 * @brief Send a "no target" reply when the command has no resolvable target.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if queued, negative errno on failure.
 * @thread_safety Loop thread only.
 */
int cmd_reply_no_target(struct cmd_ctx *ctx);

#endif /* TG_CMD_UTIL_H */
