# /blacklist

Manage the chat's word/regex blacklist: matching messages are deleted, the
sender is warned, and the configured warn action fires at the warn limit
(SPEC sections 5.2, 5.3).

## Examples

```text
/blacklist add scam
/blacklist add /free\s+crypto/i
/blacklist del scam
/blacklist list
```

## Edge cases

- `add` and `del` take a single word or a regular expression wrapped in slashes
  (`/.../ `); a plain word matches as a case-insensitive substring.
- Regex patterns are compiled with PCRE2 under match and depth limits, so a
  pathological pattern cannot stall the event loop; an invalid pattern never
  matches.
- Messages are matched against the text, or the caption for media messages.
- A match deletes the message, records a `blacklist` action and adds a warning;
  at the chat's `warn_limit` the configured `warn_action` is applied and the
  warnings are cleared, exactly like `/warn`.
- `list` shows the stored patterns (regular expressions are shown between
  slashes).
- The bot must hold `can_delete_messages` and `can_restrict_members`.
