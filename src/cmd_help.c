/* cmd_help.c - /help command handler.
 *
 * SPEC-ADDENDUM-SSOT S8 step 2: every commands.def row needs a cmd_<name> symbol.
 * The router that would invoke this handler lands in M3, so for M0 the handler is
 * a documented placeholder that reports the router is not yet wired rather than
 * silently succeeding (SPEC section 9: no silent failures).
 *
 * @file cmd_help.c
 * @brief /help handler (placeholder until the router lands in M3).
 * @ingroup commands
 */
#include <errno.h>

#include "commands.h"

int cmd_help(cmd_ctx_t *ctx)
{
    (void)ctx;
    /* The router and message context arrive in M3; until then /help cannot run. */
    return -ENOSYS;
}
