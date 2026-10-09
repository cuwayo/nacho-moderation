# Commands

Every command is registered in `src/commands.def` and documented below. This file
is checked against the command table by `scripts/check_consistency.py` (C2, C3).

<!-- BEGIN GENERATED: commands-reference -->
### /help

Show available commands.

- **Usage:** `/help `
- **Minimum role:** ROLE_ANYONE
- **Caller right:** RIGHT_NONE
- **Bot right:** RIGHT_NONE
- **Flags:** -

# /help

Show the list of commands available to the caller.

## Examples

```text
/help
```

Replies with a list of the commands the caller is allowed to use.

## Edge cases

- In a private chat, only commands available to anyone are listed.
- Commands marked hidden (`CMDF_HIDDEN`) are never shown.
<!-- END GENERATED: commands-reference -->
