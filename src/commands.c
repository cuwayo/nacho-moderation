/* commands.c - command registry and lookup derived from src/commands.def.
 *
 * SPEC-ADDENDUM-SSOT S2. cmd_table is generated from the X-macro table so the
 * enum, the handler prototypes, the descriptor table, and (in M3) /help and the
 * setMyCommands payload can never drift apart.
 *
 * @file commands.c
 * @brief Command descriptor table and name lookup.
 * @ingroup commands
 */
#include "commands.h"

#include <string.h>

/* Table generated from the single source of truth (src/commands.def). */
const cmd_desc_t cmd_table[CMD_COUNT] = {
#define X(id, name, role, ur, br, flags, usage, brief, handler)                                    \
    {CMD_##id, name, role, ur, br, flags, usage, brief, handler},
#include "commands.def"
#undef X
};

const cmd_desc_t *cmd_lookup(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < (size_t)CMD_COUNT; ++i) {
        if (strcmp(cmd_table[i].name, name) == 0) {
            return &cmd_table[i];
        }
    }
    return NULL;
}
