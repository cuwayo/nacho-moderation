# /setlog

Set or clear the moderation log channel (SPEC section 5.5).

## Examples

```text
/setlog -1001234567890
```

Sends every subsequent moderation action to that channel as a log message.

```text
/setlog off
```

Disables the moderation log for this chat.

## Edge cases

- Owner-only: this is the one command restricted to the chat creator.
- The argument is a numeric chat id, or the literal `off`.
- The log message records the action, target, admin, reason and (for a temporary
  action) the expiry time, and is sent with `parse_mode=HTML`.
- The log channel is per chat and persisted in the `chats` table.
