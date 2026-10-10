# /unpin

Unpin the most recent pinned message in the chat.

## Examples

```text
/unpin
```

Calls `unpinChatMessage` without a `message_id`, which unpins the latest pinned
message.

## Edge cases

- The bot must hold `can_pin_messages`.
- If nothing is pinned, Telegram treats the call as a no-op.
- To unpin a specific message, reply to it before the pin (the message id is not
  passed in this milestone).
