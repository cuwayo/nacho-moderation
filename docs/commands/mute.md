# /mute

Restrict a user from sending messages. With a duration the restriction is lifted
automatically when it expires (SPEC section 5.6).

## Examples

```text
/mute 30m flooding
```

Mutes the replied-to user for thirty minutes and replies `muted`.

```text
/mute @loud 2h
```

## Edge cases

- The restriction clears every send permission (`can_send_messages` and the media
  variants) so the user can only read.
- A temporary mute stores an absolute `until_date`; the expiry sweeper restores
  the common group permissions when it elapses, and the mute survives a restart.
- No duration means a permanent mute (until `/unmute`).
- The owner, other administrators and the bot itself cannot be muted.
