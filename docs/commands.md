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
  id, then an `@username`. An `@username` is matched against the chat's
  administrators (from `getChatAdministrators`) and the `users` cache, which is
  populated from every message the bot sees — including the sender of the replied-to
  message. An unknown username gets a reply suggesting a reply or a numeric id;
  Telegram offers no way to look up a user who has never been observed.
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

### /warn

Warn a user; auto-act at the limit.

- **Usage:** `/warn [target] [reason]`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_RESTRICT
- **Bot right:** RIGHT_RESTRICT
- **Flags:** CMDF_TARGET, CMDF_REASON, CMDF_GROUP_ONLY

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

### /unwarn

Remove a user's newest warning.

- **Usage:** `/unwarn [target]`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_RESTRICT
- **Bot right:** RIGHT_NONE
- **Flags:** CMDF_TARGET, CMDF_GROUP_ONLY

# /unwarn

Remove the newest warning for a user.

## Examples

```text
/unwarn
```

Removes the replied-to user's most recent warning and replies when it is done.

```text
/unwarn @spammer
```

## Edge cases

- Removes only the newest warning; use `/resetwarns` to clear them all.
- The target is resolved from a reply, a numeric id, or an `@username`.
- If the user has no warnings the call is a harmless no-op.
- The bot must hold `can_restrict_members`.

### /warns

List your warnings, or a user's (admins).

- **Usage:** `/warns [target]`
- **Minimum role:** ROLE_ANYONE
- **Caller right:** RIGHT_NONE
- **Bot right:** RIGHT_NONE
- **Flags:** CMDF_TARGET, CMDF_GROUP_ONLY

# /warns

List warnings: your own, or (for administrators) another user's.

## Examples

```text
/warns
```

Lists your own warnings, so any member can check their standing:

```text
You have 2 warning(s):
1. repeated advertising
2. spamming links
```

```text
/warns @spammer
```

Lists another user's warnings; this form is admin-only.

## Edge cases

- With no target the command is self-service: any member sees their own warnings.
- Naming another user (a reply, `@username` or numeric id) requires an
  administrator, owner or sudo caller; others get a refusal.
- At most one page (16 warnings) is listed; `/resetwarns` clears them all.
- A warning without a reason is shown as `(no reason)`.
- Read-only: listing does not require any Telegram right.

### /resetwarns

Clear all of a user's warnings.

- **Usage:** `/resetwarns [target]`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_RESTRICT
- **Bot right:** RIGHT_NONE
- **Flags:** CMDF_TARGET, CMDF_GROUP_ONLY

# /resetwarns

Clear every warning for a user.

## Examples

```text
/resetwarns
```

Clears all warnings for the replied-to user and replies when it is done.

```text
/resetwarns @spammer
```

## Edge cases

- Clears the whole history for that user, unlike `/unwarn` which removes one.
- The target is resolved from a reply, a numeric id, or an `@username`.
- The bot must hold `can_restrict_members`.

### /setwarnlimit

Set how many warnings trigger the action.

- **Usage:** `/setwarnlimit <n>`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_RESTRICT
- **Bot right:** RIGHT_NONE
- **Flags:** CMDF_GROUP_ONLY

# /setwarnlimit

Set how many warnings trigger the configured action.

## Examples

```text
/setwarnlimit 5
```

Sets the limit to five warnings; the next `/warn` that reaches five applies the
configured `warn_action` and clears the warnings.

```text
/setwarnlimit 0
```

Disables automatic action: warnings accumulate but nothing is applied
automatically.

## Edge cases

- The argument is a plain integer between 0 and 1000; anything else is rejected
  with the usage line.
- The value is per chat and persisted in the `chats` table.
- The limit is combined with `/setwarnaction`; see `/warn` for the auto-action.
- The bot must hold `can_restrict_members`.

### /setwarnaction

Set the action taken at the warning limit.

- **Usage:** `/setwarnaction ban|mute|kick`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_RESTRICT
- **Bot right:** RIGHT_NONE
- **Flags:** CMDF_GROUP_ONLY

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

### /setlog

Set or clear the moderation log channel.

- **Usage:** `/setlog <chat_id|off>`
- **Minimum role:** ROLE_OWNER
- **Caller right:** RIGHT_NONE
- **Bot right:** RIGHT_NONE
- **Flags:** CMDF_GROUP_ONLY

# /setlog

Set or clear the moderation log channel (SPEC section 5.5).

## Examples

```text
/setlog -1001234567890
```

Sends every subsequent moderation action to that channel as a log message.

```text
/setlog off
```

Disables the moderation log for this chat.

## Edge cases

- Owner-only: this is the one command restricted to the chat creator.
- The argument is a numeric chat id, or the literal `off`.
- The log message records the action, target, admin, reason and (for a temporary
  action) the expiry time, and is sent with `parse_mode=HTML`.
- The log channel is per chat and persisted in the `chats` table.

### /reload

Refresh the cached administrator list.

- **Usage:** `/reload `
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_NONE
- **Bot right:** RIGHT_NONE
- **Flags:** CMDF_GROUP_ONLY

# /reload

Refresh the cached administrator list for this chat.

## Examples

```text
/reload
```

Drops the cached `getChatAdministrators` result so the next command re-fetches
it, then confirms.

## Edge cases

- The administrator list is cached for 10 minutes and invalidated automatically on
  a `chat_member` update; `/reload` forces it immediately.
- Useful after granting or revoking a right so the bot's permission checks see the
  change at once.
- Any administrator may run it (it needs no Telegram right).

### /settings

Open the inline settings panel.

- **Usage:** `/settings `
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_CHANGE_INFO
- **Bot right:** RIGHT_NONE
- **Flags:** CMDF_GROUP_ONLY

# /settings

Open an inline-keyboard panel for the chat's settings.

## Examples

```text
/settings
```

Sends a panel with a button showing the current welcome state:

```text
Chat settings
[ Welcome: OFF ]
```

Pressing the button toggles the welcome, updates the panel in place and shows a
short confirmation.

## Edge cases

- The panel is owned by the user who opened it: a button forwarded to someone else
  is rejected ("This panel is not yours."), per SPEC section 10.
- Only administrators holding `can_change_info` may toggle settings; others get a
  refusal toast.
- Each press is acknowledged with `answerCallbackQuery` and the panel is edited
  with `editMessageText`, so the chat is not spammed with new messages.
- The panel is per chat and requires the bot to hold no specific right beyond the
  caller's `can_change_info` check.

### /setwelcome

Set the join welcome message.

- **Usage:** `/setwelcome <text>`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_CHANGE_INFO
- **Bot right:** RIGHT_NONE
- **Flags:** CMDF_GROUP_ONLY

# /setwelcome

Set the message sent when a new member joins (SPEC section 5.4).

## Examples

```text
/setwelcome Welcome {first} to {chat}! You are member #{count}.
```

Saves the template; enable delivery with `/welcome on`.

## Edge cases

- Placeholders: `{first}` (first name), `{mention}` (a mention link),
  `{chat}` (chat title) and `{count}` (member count). An unknown `{...}` token is
  left as-is.
- Substituted names are HTML-escaped, so a name cannot inject markup; the template
  itself may contain Telegram HTML.
- Saving a template does not enable delivery; use `/welcome on`.
- The template is per chat and persisted in the `chats` table.
- The bot must hold `can_change_info`.

### /welcome

Enable or disable the join welcome.

- **Usage:** `/welcome on|off`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_CHANGE_INFO
- **Bot right:** RIGHT_NONE
- **Flags:** CMDF_GROUP_ONLY

# /welcome

Enable or disable the join welcome message.

## Examples

```text
/welcome on
```

Enables delivery of the saved welcome template when a member joins.

```text
/welcome off
```

Disables it without discarding the template.

## Edge cases

- The argument is exactly `on` or `off`; anything else is rejected.
- The welcome is delivered on join for both supergroups (`chat_member`) and basic
  groups (`new_chat_members`), using the template set with `/setwelcome`.
- A join by another bot never triggers a welcome.
- The bot must hold `can_change_info`.

### /setrules

Set the chat rules.

- **Usage:** `/setrules <text>`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_CHANGE_INFO
- **Bot right:** RIGHT_NONE
- **Flags:** CMDF_GROUP_ONLY

# /setrules

Set the chat rules text.

## Examples

```text
/setrules Be kind. No spam. English or Indonesian only.
```

Saves the rules; members can read them with `/rules`.

## Edge cases

- The rules are free text and are shown verbatim by `/rules`.
- Saving overwrites any previous rules.
- The rules are per chat and persisted in the `chats` table.
- The bot must hold `can_change_info`.

### /rules

Show the chat rules.

- **Usage:** `/rules `
- **Minimum role:** ROLE_ANYONE
- **Caller right:** RIGHT_NONE
- **Bot right:** RIGHT_NONE
- **Flags:** CMDF_GROUP_ONLY

# /rules

Show the chat rules.

## Examples

```text
/rules
```

Replies with the rules set by `/setrules`, or a note that none are set.

## Edge cases

- Available to any member: unlike `/setrules`, it needs no right, so ordinary
  members can read the rules.
- If no rules have been set, the bot replies that none are set.
- The rules text is HTML-escaped before sending, so angle brackets and ampersands
  are shown literally rather than interpreted as markup.

### /lock

Lock a message type (delete + log).

- **Usage:** `/lock <type> [type...]`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_DELETE
- **Bot right:** RIGHT_DELETE
- **Flags:** CMDF_GROUP_ONLY

# /lock

Lock one or more message types: matching messages from non-administrators are
deleted and recorded in the moderation log (SPEC section 5.3).

## Examples

```text
/lock links
/lock media stickers
/lock forwards, polls
```

## Edge cases

- Lock types are `links`, `media`, `stickers`, `forwards`, `polls`, `invites`,
  `bots` and `commands`; several may be given separated by spaces or commas.
- The lock bitmask is stored per chat in the `chats.locks` column and persists
  across restarts.
- Administrators, the owner and sudo users are never affected; the bot's own
  messages and service messages are ignored.
- `links` covers URL/text-link/mention entities; `invites` also matches
  `t.me/joinchat` and `t.me/+` links in the text; `commands` covers a leading
  slash command from a non-administrator.
- An unknown lock type is rejected without changing anything.
- The bot must hold `can_delete_messages`.

### /unlock

Unlock a message type.

- **Usage:** `/unlock <type> [type...]`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_DELETE
- **Bot right:** RIGHT_DELETE
- **Flags:** CMDF_GROUP_ONLY

# /unlock

Unlock one or more previously locked message types (SPEC section 5.3).

## Examples

```text
/unlock links
/unlock media stickers
```

## Edge cases

- Unlocking a type that is not currently locked is a no-op (the bitmask is
  simply cleared).
- The remaining locked types are unaffected.
- Lock types are the same set as `/lock`: `links`, `media`, `stickers`,
  `forwards`, `polls`, `invites`, `bots`, `commands`.
- An unknown lock type is rejected without changing anything.
- The bot must hold `can_delete_messages`.

### /blacklist

Manage the chat word/regex blacklist.

- **Usage:** `/blacklist add|del|list <word|/regex/>`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_DELETE
- **Bot right:** RIGHT_DELETE
- **Flags:** CMDF_GROUP_ONLY

# /blacklist

Manage the chat's word/regex blacklist: matching messages are deleted, the
sender is warned, and the configured warn action fires at the warn limit
(SPEC sections 5.2, 5.3).

## Examples

```text
/blacklist add scam
/blacklist add /free\s+crypto/i
/blacklist del scam
/blacklist list
```

## Edge cases

- `add` and `del` take a single word or a regular expression wrapped in slashes
  (`/.../ `); a plain word matches as a case-insensitive substring.
- Regex patterns are compiled with PCRE2 under match and depth limits, so a
  pathological pattern cannot stall the event loop; an invalid pattern never
  matches.
- Messages are matched against the text, or the caption for media messages.
- A match deletes the message, records a `blacklist` action and adds a warning;
  at the chat's `warn_limit` the configured `warn_action` is applied and the
  warnings are cleared, exactly like `/warn`.
- `list` shows the stored patterns (regular expressions are shown between
  slashes).
- The bot must hold `can_delete_messages` and `can_restrict_members`.

### /antiflood

Set the flood threshold, or turn it off.

- **Usage:** `/antiflood <messages> <seconds>|off`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_RESTRICT
- **Bot right:** RIGHT_RESTRICT
- **Flags:** CMDF_GROUP_ONLY

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

### /captcha

Enable or disable join verification.

- **Usage:** `/captcha on|off`
- **Minimum role:** ROLE_ADMIN
- **Caller right:** RIGHT_RESTRICT
- **Bot right:** RIGHT_RESTRICT
- **Flags:** CMDF_GROUP_ONLY

# /captcha

Enable or disable join verification: new members must press an "I'm human"
button within a timeout or they are removed (SPEC section 5.4).

## Examples

```text
/captcha on
/captcha off
```

## Edge cases

- With the captcha enabled a new member is immediately restricted (they cannot
  send messages), sent an inline "I'm human" button and recorded in
  `pending_captcha` with a deadline (default 120 seconds).
- Pressing the button as the challenged user lifts the restriction, deletes the
  prompt and delivers the welcome message if one is configured.
- Pressing the button as any other user is rejected (the callback data carries
  the challenged user id, checked against the presser).
- When the deadline passes a periodic sweep kicks the member (ban then unban, so
  they may rejoin) and deletes the prompt.
- Both supergroup (`chat_member`) and basic-group (`new_chat_members`) joins are
  handled.
- The bot must hold `can_restrict_members`.
<!-- END GENERATED: commands-reference -->
