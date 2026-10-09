/* commands.h - command registry derived from src/commands.def.
 *
 * SPEC-ADDENDUM-SSOT S2. The command set lives in exactly one place: the
 * X-macro table src/commands.def. This header turns it into the CMD_* enum and
 * the handler prototypes; commands.c turns it into cmd_table and lookup. No
 * command name is written anywhere else in C (S1, S10).
 *
 * @file commands.h
 * @brief Command identifiers, handler prototypes, and the command descriptor table.
 * @ingroup commands
 */
#ifndef NM_COMMANDS_H
#define NM_COMMANDS_H

#include <stddef.h>

#include "tables.h"

/** @brief Per-invocation command context (filled in from M3). */
typedef struct cmd_ctx cmd_ctx_t;

/** @brief Command identifiers, one per commands.def row, plus CMD_COUNT. */
typedef enum {
#define X(id, name, role, ur, br, flags, usage, brief, handler) CMD_##id,
#include "commands.def"
#undef X
    CMD_COUNT /**< Number of registered commands. */
} cmd_id_t;

/** @brief Static description of one command. All pointers are to string literals. */
typedef struct {
    cmd_id_t id;                 /**< Stable identifier (CMD_<id>). */
    const char *name;            /**< Telegram command name without the leading '/'. */
    role_t min_role;             /**< Minimum caller role. */
    right_t user_right;          /**< Right the caller must hold (owner/sudo bypass). */
    right_t bot_right;           /**< Right the bot must hold in the chat. */
    unsigned flags;              /**< CMDF_* bitmask. */
    const char *usage;           /**< Argument synopsis ("" if none). */
    const char *brief;           /**< One-line description for /help and setMyCommands. */
    int (*handler)(cmd_ctx_t *); /**< Entry point; never NULL. */
} cmd_desc_t;

/* Handler prototypes, one per commands.def row. */
#define X(id, name, role, ur, br, flags, usage, brief, handler) int handler(cmd_ctx_t *ctx);
#include "commands.def"
#undef X

/** @brief The command table in commands.def order (length CMD_COUNT). */
extern const cmd_desc_t cmd_table[CMD_COUNT];

/**
 * @brief Look up a command by name (case-sensitive, no leading '/').
 *
 * @param[in] name Command name to find.
 *
 * @return Pointer to the descriptor, or NULL if no command matches.
 * @ownership Returned pointer has static storage; never freed.
 * @thread_safety Read-only; safe from any thread.
 */
const cmd_desc_t *cmd_lookup(const char *name);

#endif /* NM_COMMANDS_H */
