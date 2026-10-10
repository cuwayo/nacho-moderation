# /del

Delete the replied-to message and the command message.

## Examples

```text
/del
```

Used as a reply; issues two `deleteMessage` calls (the replied-to message and the
command).

## Edge cases

- Must be used as a reply; otherwise the bot replies that a reply is required.
- The bot must hold `can_delete_messages`.
- Both deletions are issued immediately; a failure of one does not block the
  other.
- The deleted message's sender is recorded as the target of the `del` action.
