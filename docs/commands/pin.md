# /pin

Pin the replied-to message.

## Examples

```text
/pin
```

Used as a reply; pins the replied-to message silently
(`disable_notification=true`).

## Edge cases

- Must be used as a reply; otherwise the bot replies that a reply is required.
- The bot must hold `can_pin_messages`.
- The pin is silent so it does not notify the chat.
