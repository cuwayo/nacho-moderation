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
#include "tg/util.h"

/** @brief Per-invocation command context (defined in tg/cmd_ctx.h). */
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

/**
 * @brief Render the /help text for a caller into @p out.
 *
 * Lists every non-hidden command whose @c min_role is at most @p role (owner and
 * sudo see everything). Rendered entirely from @ref cmd_table so the text cannot
 * drift from the command set (SPEC-ADDENDUM-SSOT S2).
 *
 * @param[in]  role     Caller's role.
 * @param[in]  is_group 1 if the chat is a group/supergroup (hides group-only commands in private).
 * @param[out] out      Builder to append to (its prior contents are preserved).
 *
 * @return 0 on success, -ENOMEM on allocation failure.
 * @ownership @p out is caller-owned; the text is valid until the next append/free.
 * @thread_safety Safe (reads only the static table).
 */
int commands_render_help(role_t role, int is_group, strbuf_t *out);

/**
 * @brief Build the `setMyCommands` JSON payload from @ref cmd_table.
 *
 * Emits `{"commands":[{"command":...,"description":...},...]}` for every
 * non-hidden command (SPEC-ADDENDUM-SSOT S2). Telegram limits each command name to
 * 32 bytes and each description to 256, both guaranteed by the table test.
 *
 * @param[out] out    Output buffer.
 * @param[in]  cap    Capacity of @p out.
 * @param[out] outlen Receives the payload length (excluding NUL).
 *
 * @return 0 on success, -ENOSPC if @p out is too small, -EINVAL on bad arguments.
 * @thread_safety Safe (reads only the static table).
 */
int commands_build_set_my_commands(char *out, size_t cap, size_t *outlen);

#endif /* NM_COMMANDS_H */
