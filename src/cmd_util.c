/**
 * @file cmd_util.c
 * @brief Shared helpers for the moderation command handlers (see cmd_util.h).
 *
 * @ingroup commands
 */
#include "tg/cmd_util.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "tg/cmd_ctx.h"
#include "tg/perms.h"
#include "tg/util.h"

/**
 * @brief Return @p s with leading spaces and tabs skipped.
 *
 * @param[in] s String.
 * @return Pointer to the first non-space byte.
 */
static const char *skip_spaces(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

int cmd_parse_duration_reason(const char *args, int64_t *duration_s, const char **reason)
{
    if (args == NULL || duration_s == NULL || reason == NULL) {
        return -EINVAL;
    }
    *duration_s = 0;
    const char *p = skip_spaces(args);

    /* The first token is a duration only if it parses as one; otherwise the whole
     * tail is the reason (so "reason 10m" is not mistaken for a duration). */
    size_t tok_len = 0;
    while (p[tok_len] != '\0' && !isspace((unsigned char)p[tok_len])) {
        tok_len++;
    }
    if (tok_len > 0 && tok_len < 32) {
        char tok[32];
        (void)memcpy(tok, p, tok_len);
        tok[tok_len] = '\0';
        int64_t seconds = 0;
        if (util_parse_duration(tok, &seconds) == 0) {
            *duration_s = seconds;
            p += tok_len;
            p = skip_spaces(p);
        }
    }
    *reason = p;
    return 0;
}

int cmd_target_guard(const struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    return perms_target_ok(ctx->target_role, ctx->target_id, ctx->bot_id);
}

int cmd_reply_no_target(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    return cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                               "Reply to a user, or give an @username or numeric user id.");
}
