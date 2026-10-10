# /reload

Refresh the cached administrator list for this chat.

## Examples

```text
/reload
```

Drops the cached `getChatAdministrators` result so the next command re-fetches
it, then confirms.

## Edge cases

- The administrator list is cached for 10 minutes and invalidated automatically on
  a `chat_member` update; `/reload` forces it immediately.
- Useful after granting or revoking a right so the bot's permission checks see the
  change at once.
- Any administrator may run it (it needs no Telegram right).
