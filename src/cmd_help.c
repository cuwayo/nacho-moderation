/* cmd_help.c - /help command handler.
 *
 * Renders the command list from cmd_table (SPEC-ADDENDUM-SSOT S2) and replies via
 * the context's reply hook, which the router implements with sendMessage. The
 * handler itself never touches the network or the loop directly.
 *
 * @file cmd_help.c
 * @brief /help handler: reply with the commands available to the caller.
 * @ingroup commands
 */
#include <errno.h>

#include "commands.h"
#include "tables.h"
#include "tg/cmd_ctx.h"
#include "tg/util.h"

int cmd_help(struct cmd_ctx *ctx)
{
    if (ctx == NULL || ctx->reply == NULL) {
        return -EINVAL;
    }

    role_t role = ctx->is_sudo ? ROLE_SUDO : ctx->role;
    strbuf_t out;
    int rc = strbuf_init(&out, 256);
    if (rc != 0) {
        return rc;
    }
    rc = commands_render_help(role, ctx->is_group, &out);
    if (rc == 0) {
        rc = ctx->reply(ctx, strbuf_cstr(&out), ctx->msg != NULL ? ctx->msg->message_id : 0);
    }
    strbuf_free(&out);
    return rc;
}
