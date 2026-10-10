# /purge

Delete every message from the replied-to message up to the command, in
`deleteMessages` batches of 100 (SPEC section 5.2).

## Examples

```text
/purge
```

Used as a reply; deletes the replied-to message, everything after it, and the
command itself.

## Edge cases

- Must be used as a reply; otherwise the bot replies that a reply is required.
- Ids are collected from the router's bounded per-chat ring of recent messages and
  split into batches of at most 100 (Telegram's limit).
- At most 1000 messages are deleted in one `/purge`.
- The bot must hold `can_delete_messages`.
- Messages the bot cannot see (or that were already deleted) are skipped by
  Telegram; a batch failure is logged but does not stop the remaining batches.
