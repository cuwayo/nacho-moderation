# /warn

Record a warning against a user and, when the chat's warning limit is reached,
apply the configured action and clear the warnings (SPEC section 5.2).

## Examples

```text
/warn spamming links
```

Warns the replied-to user with the reason "spamming links" and replies with the
new warning count.

```text
/warn @spammer repeated advertising
/warn 123456789
```

## Edge cases

- The target is resolved from a reply, a numeric id, or an `@username`, exactly
  like the other moderation commands.
- The warning is stored in the `warns` table with the caller as `admin_id`, the
  reason and a timestamp.
- When the count reaches the chat's `warn_limit` (set with `/setwarnlimit`) the
  configured `warn_action` (`ban`, `mute` or `kick`, set with `/setwarnaction`) is
  applied and the warnings are cleared. A `warn_limit` of `0` disables auto-action.
- The auto-action reuses the matching moderation command, so its target guard,
  API call and `actions` row are identical to a manual `/ban`, `/mute` or `/kick`.
- The owner, other administrators and the bot itself cannot be warned.
- The bot must hold `can_restrict_members`.
