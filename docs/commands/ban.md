# /ban

Ban a user from the chat. With a duration the ban is temporary and is lifted
automatically when it expires (SPEC section 5.6); without one it is permanent.

## Examples

```text
/ban 10m spamming
```

Bans the replied-to user for ten minutes, records the action, and replies
`banned`.

```text
/ban @spammer 7d repeated advertising
/ban 123456789
```

## Edge cases

- The target is resolved from the replied-to message first, then a numeric user
  id, then an `@username`. An `@username` is matched against the chat's
  administrators (from `getChatAdministrators`) and the `users` cache, which is
  populated from every message the bot sees — including the sender of the replied-to
  message. An unknown username gets a reply suggesting a reply or a numeric id;
  Telegram offers no way to look up a user who has never been observed.
- The owner, other administrators and the bot itself cannot be banned; the bot
  replies that it cannot act on them.
- `until_date` is an absolute wall-clock timestamp, so a temporary ban survives a
  restart and is lifted by the expiry sweeper even after the process restarts.
- A bare `0` or a missing duration means a permanent ban.
- The reason is free text after the duration and is stored with the action.
