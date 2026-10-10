# /rules

Show the chat rules.

## Examples

```text
/rules
```

Replies with the rules set by `/setrules`, or a note that none are set.

## Edge cases

- Available to any member: unlike `/setrules`, it needs no right, so ordinary
  members can read the rules.
- If no rules have been set, the bot replies that none are set.
- The rules text is HTML-escaped before sending, so angle brackets and ampersands
  are shown literally rather than interpreted as markup.
