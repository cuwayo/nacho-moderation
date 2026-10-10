# /unmute

Restore a user's ability to send messages after a `/mute`.

## Examples

```text
/unmute @loud
/unmute 123456789
```

Restores the common group permissions and replies `unmuted`.

## Edge cases

- Restores the standard group permissions (messages, media, polls, previews and
  invites); it does not restore administrative rights.
- The target is resolved from a reply, a numeric id, or an `@username`.
- The bot must hold `can_restrict_members`.
