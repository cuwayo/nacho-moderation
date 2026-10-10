# Commands

Every command is registered in `src/commands.def` and documented below. This file
is checked against the command table by `scripts/check_consistency.py` (C2, C3).

<!-- BEGIN GENERATED: commands-reference -->
### /help

Show available commands.

- **Usage:** `/help `
- **Minimum role:** ROLE_ANYONE
- **Caller right:** RIGHT_NONE
- **Bot right:** RIGHT_NONE
- **Flags:** -

# /help

Show the list of commands available to the caller.

## Examples

```text
/help
```

Replies with the commands the caller is allowed to use, rendered from the command
table (`src/commands.def`), for example:

```text
Available commands:
/help - Show available commands
```

The reply is sent with `parse_mode=HTML` as a reply to the command message.

## Edge cases

- The list is filtered by the caller's resolved role: a member sees only commands
  with `ROLE_ANYONE`, an administrator sees admin commands too, and the owner and
  sudo users see everything.
- In a private chat, the caller is treated as a plain member (Telegram does not
  expose an administrator list for private chats, so `getChatAdministrators` is
  never called there), and commands flagged `CMDF_GROUP_ONLY` are omitted.
- Commands flagged `CMDF_HIDDEN` are never listed.
- `/help@MyBot` is accepted; `/help@OtherBot` is ignored (it addresses another bot).
- In a group, the caller's role is resolved with `getChatAdministrators`, cached
  for 10 minutes and invalidated on a `chat_member` update or `/reload`. If that
  lookup fails, the bot replies that it could not verify permissions rather than
  silently dropping the command.

### /ban

Ban a user from the chat.

- **Usage:** `/ban [target] [duration] [reason]`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_RESTRICT
- **Bot right:** RIGHT_RESTRICT
- **Flags:** CMDF_TARGET, CMDF_DURATION, CMDF_REASON, CMDF_GROUP_ONLY

# /ban

Ban a user from the chat. With a duration the ban is temporary and is lifted
automatically when it expires (SPEC section 5.6); without one it is permanent.

## Examples

```text
/ban 10m spamming
```

Bans the replied-to user for ten minutes, records the action, and replies
`banned`.

```text
/ban @spammer 7d repeated advertising
/ban 123456789
```

## Edge cases

- The target is resolved from the replied-to message first, then a numeric user
  id, then an `@username` (resolved from the `users` cache; an unknown username
  gets a reply asking the user to speak first).
- The owner, other administrators and the bot itself cannot be banned; the bot
  replies that it cannot act on them.
- `until_date` is an absolute wall-clock timestamp, so a temporary ban survives a
  restart and is lifted by the expiry sweeper even after the process restarts.
- A bare `0` or a missing duration means a permanent ban.
- The reason is free text after the duration and is stored with the action.

### /unban

Lift a ban.

- **Usage:** `/unban [target]`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_RESTRICT
- **Bot right:** RIGHT_RESTRICT
- **Flags:** CMDF_TARGET, CMDF_GROUP_ONLY

# /unban

Lift a ban so the user may join again.

## Examples

```text
/unban @spammer
/unban 123456789
```

Calls `unbanChatMember` with `only_if_banned=true` and replies `unbanned`.

## Edge cases

- Works on a user who is currently banned; if they are not banned Telegram treats
  the call as a no-op.
- The target is resolved from a reply, a numeric id, or an `@username`.
- The bot must hold `can_restrict_members`; otherwise it replies that it lacks the
  rights.

### /kick

Remove a user so they can rejoin.

- **Usage:** `/kick [target] [reason]`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_RESTRICT
- **Bot right:** RIGHT_RESTRICT
- **Flags:** CMDF_TARGET, CMDF_REASON, CMDF_GROUP_ONLY

# /kick

Remove a user from the chat but let them rejoin. Implemented as a ban followed
immediately by an unban in the ban's completion callback (SPEC section 5.2).

## Examples

```text
/kick @noise
```

Kicks the user and replies `kicked`.

## Edge cases

- The two API calls are chained: the unban is only issued after the ban succeeds.
  If the ban fails, the bot reports the error and does not unban.
- The invocation is reference-counted, so the unban callback safely runs after the
  handler has returned (SPEC section 9).
- The owner, other administrators and the bot itself cannot be kicked.
- `/kick` takes no duration; it never leaves the user banned.

### /mute

Restrict a user from sending messages.

- **Usage:** `/mute [target] [duration] [reason]`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_RESTRICT
- **Bot right:** RIGHT_RESTRICT
- **Flags:** CMDF_TARGET, CMDF_DURATION, CMDF_REASON, CMDF_GROUP_ONLY

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

### /unmute

Restore a user's permissions.

- **Usage:** `/unmute [target]`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_RESTRICT
- **Bot right:** RIGHT_RESTRICT
- **Flags:** CMDF_TARGET, CMDF_GROUP_ONLY

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

### /del

Delete the replied-to message and the command.

- **Usage:** `/del `
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_DELETE
- **Bot right:** RIGHT_DELETE
- **Flags:** CMDF_REPLY_REQUIRED, CMDF_GROUP_ONLY

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

### /pin

Pin the replied-to message.

- **Usage:** `/pin `
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_PIN
- **Bot right:** RIGHT_PIN
- **Flags:** CMDF_REPLY_REQUIRED, CMDF_GROUP_ONLY

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

### /unpin

Unpin the last pinned message.

- **Usage:** `/unpin `
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_PIN
- **Bot right:** RIGHT_PIN
- **Flags:** CMDF_GROUP_ONLY

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

### /purge

Delete messages from the replied message to now.

- **Usage:** `/purge `
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_DELETE
- **Bot right:** RIGHT_DELETE
- **Flags:** CMDF_REPLY_REQUIRED, CMDF_GROUP_ONLY

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
- Telegram message ids are sequential per chat, so the bot deletes the whole id
  range from the replied-to message up to and including the command, split into
  batches of at most 100 (Telegram's `deleteMessages` limit). This works for
  messages sent before the bot started or while it was offline, not only for
  messages the bot happened to observe.
- At most 1000 messages are deleted in one `/purge`; if the range is larger, the
  newest 1000 are deleted.
- The bot must hold `can_delete_messages`.
- Messages the bot cannot see (or that were already deleted) are skipped by
  Telegram; a batch failure is logged but does not stop the remaining batches.
<!-- END GENERATED: commands-reference -->
