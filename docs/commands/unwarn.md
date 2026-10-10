# /unwarn

Remove the newest warning for a user.

## Examples

```text
/unwarn
```

Removes the replied-to user's most recent warning and replies when it is done.

```text
/unwarn @spammer
```

## Edge cases

- Removes only the newest warning; use `/resetwarns` to clear them all.
- The target is resolved from a reply, a numeric id, or an `@username`.
- If the user has no warnings the call is a harmless no-op.
- The bot must hold `can_restrict_members`.
