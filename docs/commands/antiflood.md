# /antiflood

Configure flood control: when a user sends more than the threshold within the
window, they are muted and their burst messages are deleted (SPEC section 5.3).

## Examples

```text
/antiflood 5 10
/antiflood 10 30
/antiflood off
```

## Edge cases

- The arguments are `<messages> <seconds>`: more than `<messages>` messages
  within a sliding `<seconds>`-second window trips the flood control.
- The window is counted in memory per `(chat_id, user_id)` and never persisted;
  a restart clears all counters.
- On a trip the user is muted for 5 minutes (with a temporary restriction that
  the expiry sweeper later lifts) and the messages still in the window are
  deleted.
- `/antiflood off` sets the threshold to 0, disabling the check.
- Administrators, the owner and sudo users are exempt.
- The bot must hold `can_restrict_members` and `can_delete_messages`.
