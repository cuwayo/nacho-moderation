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

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "tables.h"
#include "tg/util.h"

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

/** @brief Report whether @p cmd is visible to @p role in a chat of type @p is_group. */
static int command_visible(const cmd_desc_t *cmd, role_t role, int is_group)
{
    if ((cmd->flags & CMDF_HIDDEN) != 0u) {
        return 0;
    }
    if (!is_group && (cmd->flags & CMDF_GROUP_ONLY) != 0u) {
        return 0;
    }
    /* Owner and sudo see everything; everyone else sees commands up to their role. */
    if (role == ROLE_OWNER || role == ROLE_SUDO) {
        return 1;
    }
    if (cmd->min_role == ROLE_ANYONE) {
        return 1;
    }
    if (cmd->min_role == ROLE_ADMIN) {
        return role == ROLE_ADMIN;
    }
    return 0; /* ROLE_OWNER / ROLE_SUDO commands need an exact role match above */
}

int commands_render_help(role_t role, int is_group, strbuf_t *out)
{
    if (out == NULL) {
        return -EINVAL;
    }
    int rc = strbuf_append_str(out, "Available commands:\n");
    if (rc != 0) {
        return rc;
    }
    for (size_t i = 0; i < (size_t)CMD_COUNT; ++i) {
        const cmd_desc_t *cmd = &cmd_table[i];
        if (!command_visible(cmd, role, is_group)) {
            continue;
        }
        rc = strbuf_appendf(out, "/%s - %s\n", cmd->name, cmd->brief);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/** @brief Append a JSON-escaped @p s to @p out (escapes `"`, `\` and control bytes). */
static int append_json_escaped(strbuf_t *out, const char *s)
{
    for (const char *p = s; *p != '\0'; ++p) {
        char buf[8];
        const char *add = buf;
        switch (*p) {
        case '"':
            add = "\\\"";
            break;
        case '\\':
            add = "\\\\";
            break;
        case '\n':
            add = "\\n";
            break;
        case '\r':
            add = "\\r";
            break;
        case '\t':
            add = "\\t";
            break;
        default:
            buf[0] = *p;
            buf[1] = '\0';
            break;
        }
        int rc = strbuf_append_str(out, add);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/** @brief Append one `{"command":...,"description":...}` entry to @p b. */
static int append_command_entry(strbuf_t *b, const cmd_desc_t *cmd, int first)
{
    int rc = strbuf_append_str(b, first ? "{\"command\":\"" : ",{\"command\":\"");
    if (rc == 0) {
        rc = append_json_escaped(b, cmd->name);
    }
    if (rc == 0) {
        rc = strbuf_append_str(b, "\",\"description\":\"");
    }
    if (rc == 0) {
        rc = append_json_escaped(b, cmd->brief);
    }
    if (rc == 0) {
        rc = strbuf_append_str(b, "\"}");
    }
    return rc;
}

int commands_build_set_my_commands(char *out, size_t cap, size_t *outlen)
{
    if (out == NULL || outlen == NULL) {
        return -EINVAL;
    }
    strbuf_t b;
    int rc = strbuf_init(&b, 512);
    if (rc != 0) {
        return rc;
    }
    rc = strbuf_append_str(&b, "{\"commands\":[");
    int first = 1;
    for (size_t i = 0; rc == 0 && i < (size_t)CMD_COUNT; ++i) {
        const cmd_desc_t *cmd = &cmd_table[i];
        if ((cmd->flags & CMDF_HIDDEN) != 0u) {
            continue;
        }
        rc = append_command_entry(&b, cmd, first);
        first = 0;
    }
    if (rc == 0) {
        rc = strbuf_append_str(&b, "]}");
    }
    if (rc != 0) {
        strbuf_free(&b);
        return rc;
    }

    const char *text = strbuf_cstr(&b);
    size_t len = strlen(text);
    if (len + 1 > cap) {
        strbuf_free(&b);
        return -ENOSPC;
    }
    (void)memcpy(out, text, len + 1);
    *outlen = len;
    strbuf_free(&b);
    return 0;
}
