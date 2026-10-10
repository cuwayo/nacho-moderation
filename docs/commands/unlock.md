# /unlock

Unlock one or more previously locked message types (SPEC section 5.3).

## Examples

```text
/unlock links
/unlock media stickers
```

## Edge cases

- Unlocking a type that is not currently locked is a no-op (the bitmask is
  simply cleared).
- The remaining locked types are unaffected.
- Lock types are the same set as `/lock`: `links`, `media`, `stickers`,
  `forwards`, `polls`, `invites`, `bots`, `commands`.
- An unknown lock type is rejected without changing anything.
- The bot must hold `can_delete_messages`.
