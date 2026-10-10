# /lock

Lock one or more message types: matching messages from non-administrators are
deleted and recorded in the moderation log (SPEC section 5.3).

## Examples

```text
/lock links
/lock media stickers
/lock forwards, polls
```

## Edge cases

- Lock types are `links`, `media`, `stickers`, `forwards`, `polls`, `invites`,
  `bots` and `commands`; several may be given separated by spaces or commas.
- The lock bitmask is stored per chat in the `chats.locks` column and persists
  across restarts.
- Administrators, the owner and sudo users are never affected; the bot's own
  messages and service messages are ignored.
- `links` covers URL/text-link/mention entities; `invites` also matches
  `t.me/joinchat` and `t.me/+` links in the text; `commands` covers a leading
  slash command from a non-administrator.
- An unknown lock type is rejected without changing anything.
- The bot must hold `can_delete_messages`.
