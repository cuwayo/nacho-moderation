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

Replies with the commands the caller is allowed to use, rendered from the command
table (`src/commands.def`), for example:

```text
Available commands:
/help - Show available commands
```

The reply is sent with `parse_mode=HTML` as a reply to the command message.

## Edge cases

- The list is filtered by the caller's resolved role: a member sees only commands
  with `ROLE_ANYONE`, an administrator sees admin commands too, and the owner and
  sudo users see everything.
- In a private chat, the caller is treated as a plain member (Telegram does not
  expose an administrator list for private chats, so `getChatAdministrators` is
  never called there), and commands flagged `CMDF_GROUP_ONLY` are omitted.
- Commands flagged `CMDF_HIDDEN` are never listed.
- `/help@MyBot` is accepted; `/help@OtherBot` is ignored (it addresses another bot).
- In a group, the caller's role is resolved with `getChatAdministrators`, cached
  for 10 minutes and invalidated on a `chat_member` update or `/reload`. If that
  lookup fails, the bot replies that it could not verify permissions rather than
  silently dropping the command.
<!-- END GENERATED: commands-reference -->
