# /setwarnaction

Choose the action applied when a user reaches the warning limit.

## Examples

```text
/setwarnaction mute
```

Sets the auto-action to `mute`; when the limit is reached the user is muted and
their warnings are cleared.

```text
/setwarnaction ban
/setwarnaction kick
```

## Edge cases

- Accepted values are exactly `ban`, `mute` and `kick`; anything else is rejected.
- The action reuses the matching moderation command, so `/ban`'s duration and
  reason handling applies only when the action is issued manually; the
  auto-action mutes/bans permanently and kicks immediately.
- The default for a new chat is `mute`.
- The bot must hold `can_restrict_members`.
