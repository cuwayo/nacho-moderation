# /unban

Lift a ban so the user may join again.

## Examples

```text
/unban @spammer
/unban 123456789
```

Calls `unbanChatMember` with `only_if_banned=true` and replies `unbanned`.

## Edge cases

- Works on a user who is currently banned; if they are not banned Telegram treats
  the call as a no-op.
- The target is resolved from a reply, a numeric id, or an `@username`.
- The bot must hold `can_restrict_members`; otherwise it replies that it lacks the
  rights.
