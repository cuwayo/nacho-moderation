# /resetwarns

Clear every warning for a user.

## Examples

```text
/resetwarns
```

Clears all warnings for the replied-to user and replies when it is done.

```text
/resetwarns @spammer
```

## Edge cases

- Clears the whole history for that user, unlike `/unwarn` which removes one.
- The target is resolved from a reply, a numeric id, or an `@username`.
- The bot must hold `can_restrict_members`.
