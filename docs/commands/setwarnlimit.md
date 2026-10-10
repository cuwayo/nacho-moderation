# /setwarnlimit

Set how many warnings trigger the configured action.

## Examples

```text
/setwarnlimit 5
```

Sets the limit to five warnings; the next `/warn` that reaches five applies the
configured `warn_action` and clears the warnings.

```text
/setwarnlimit 0
```

Disables automatic action: warnings accumulate but nothing is applied
automatically.

## Edge cases

- The argument is a plain integer between 0 and 1000; anything else is rejected
  with the usage line.
- The value is per chat and persisted in the `chats` table.
- The limit is combined with `/setwarnaction`; see `/warn` for the auto-action.
- The bot must hold `can_restrict_members`.
