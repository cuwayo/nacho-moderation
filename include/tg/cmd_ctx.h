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
 * @ingroup commands
 */
#ifndef TG_CMD_CTX_H
#define TG_CMD_CTX_H

#include <stdint.h>

#include "commands.h"
#include "tables.h"
#include "tg/tg_types.h"

/** @brief Everything a command handler needs for one invocation. */
struct cmd_ctx {
    const cmd_desc_t *cmd;   /**< The command being run (never NULL). */
    int64_t chat_id;         /**< Chat the command was sent in. */
    int64_t user_id;         /**< Caller's user id. */
    int64_t target_id;       /**< Resolved target user id (0 if none). */
    const char *args;        /**< Text after the command and target (never NULL). */
    const tg_message_t *msg; /**< The originating message (never NULL). */
    int is_group;            /**< 1 if the chat is a group/supergroup. */
    role_t role;             /**< Caller's resolved role in the chat. */
    int is_sudo;             /**< 1 if the caller is a configured sudo user. */

    /**
     * @brief Send a reply to @p chat_id.
     *
     * Implemented by the router; wraps `sendMessage` with the given HTML text and
     * optional reply-to message id (0 for none). Never blocks.
     *
     * @param[in] ctx      The context.
     * @param[in] html     HTML-escaped message text.
     * @param[in] reply_to Message id to reply to (0 for none).
     *
     * @return 0 if queued, negative errno on failure.
     */
    int (*reply)(struct cmd_ctx *ctx, const char *html, int64_t reply_to);
    void *reply_ctx; /**< Opaque context for @ref reply (owned by the router). */
};

#endif /* TG_CMD_CTX_H */
