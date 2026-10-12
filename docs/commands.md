# Commands

Every command is declared once in `src/nacho_moderator/commands/registry.py`. The
`/help` text and the two `setMyCommands` scopes are derived from that table, so a
command cannot exist in one place but not another. This file has one
`### /<name>` section per registered command; `tests/test_docs_consistency.py`
fails if a command is missing here or a section names a command that does not
exist.

Target resolution order: the replied-to message's sender, then a numeric user id,
then an `@username` (resolved against the chat's administrators and the users
cache). A duration suffix is `30s`, `10m`, `2h`, `7d`, or `1w`; without one the
action is permanent. Trailing free text is the reason.

## Moderation

### /ban

Ban a user from the chat. A duration makes the ban temporary; without one it is
permanent. The ban is recorded and mirrored to the log channel.

**Usage:** `[target] [duration] [reason]` · **Role:** admin · **Rights:** restrict (caller and bot)

**Examples**

- `/ban` (as a reply) — ban the replied-to user permanently.
- `/ban 10m spamming` (as a reply) — ban for ten minutes, reason "spamming".
- `/ban @alice 2h` — ban a resolvable username for two hours.

**Edge cases**

- The owner, another administrator, and the bot itself cannot be banned.
- A temporary ban survives a restart and is lifted by the sweeper.

### /unban

Lift a ban (`unbanChatMember` with `only_if_banned`).

**Usage:** `[target]` · **Role:** admin · **Rights:** restrict (caller and bot)

**Examples**

- `/unban` (as a reply) — unban the replied-to user.
- `/unban 123456789` — unban by numeric id.

**Edge cases**

- Unbanning a user who is not banned is a no-op, not an error.

### /kick

Remove a user so they can rejoin (ban, then unban).

**Usage:** `[target] [reason]` · **Role:** admin · **Rights:** restrict (caller and bot)

**Examples**

- `/kick` (as a reply) — remove the replied-to user.
- `/kick @bob being disruptive` — remove with a reason.

**Edge cases**

- The owner, another administrator, and the bot itself cannot be kicked.

### /mute

Restrict a user from sending messages. A duration makes it temporary.

**Usage:** `[target] [duration] [reason]` · **Role:** admin · **Rights:** restrict (caller and bot)

**Examples**

- `/mute 30m` (as a reply) — mute for thirty minutes.
- `/mute @carol` — mute permanently.

**Edge cases**

- A temporary mute survives a restart and is lifted by the sweeper.

### /unmute

Restore a user's common group permissions.

**Usage:** `[target]` · **Role:** admin · **Rights:** restrict (caller and bot)

**Examples**

- `/unmute` (as a reply) — restore the replied-to user.

**Edge cases**

- This restores the common permissions only; it does not grant administrator
  rights.

### /promote

Grant a user a limited set of moderator rights: delete messages, restrict
members, and pin messages. It never grants `can_promote_members`,
`can_change_info`, or `can_manage_chat`. An optional custom title is trimmed to
16 characters.

**Usage:** `[target] [title]` · **Role:** admin · **Rights:** promote (caller and bot)

**Examples**

- `/promote` (as a reply) — grant the limited rights.
- `/promote Moderator` (as a reply) — grant the rights and set the title.

**Edge cases**

- The bot must itself hold `can_promote_members`.
- The title is HTML-escaped when echoed and truncated to Telegram's limit.

### /demote

Remove every administrator right from a user.

**Usage:** `[target]` · **Role:** admin · **Rights:** promote (caller and bot)

**Examples**

- `/demote` (as a reply) — demote the replied-to administrator.
- `/demote 123456789` — demote by numeric id.

**Edge cases**

- The chat owner and the bot itself are refused.
- Telegram rejects an administrator the bot did not promote; that is reported and
  never retried.

### /del

Delete the replied-to message and the command message.

**Usage:** *(reply)* · **Role:** admin · **Rights:** delete (caller and bot)

**Examples**

- `/del` (as a reply) — remove the replied-to message and the command.

**Edge cases**

- Requires a reply; otherwise the bot replies with usage.

### /pin

Pin the replied-to message silently.

**Usage:** *(reply)* · **Role:** admin · **Rights:** pin (caller and bot)

**Examples**

- `/pin` (as a reply) — pin the replied-to message.

**Edge cases**

- Requires a reply; otherwise the bot replies with usage.

### /unpin

Unpin the most recent pinned message.

**Usage:** *(none)* · **Role:** admin · **Rights:** pin (caller and bot)

**Examples**

- `/unpin` — clear the current pin.

**Edge cases**

- With nothing pinned, Telegram accepts the call and does nothing.

### /purge

Delete every message from the replied-to message up to the command, in batches of
100, capped at 1000 ids.

**Usage:** *(reply)* · **Role:** admin · **Rights:** delete (caller and bot)

**Examples**

- `/purge` (as a reply) — delete the range from the replied-to message to now.

**Edge cases**

- Requires a reply; otherwise the bot replies with usage.
- A failed batch aborts with "That failed." rather than continuing.

## Warnings

### /warn

Add a warning; at the per-chat limit the configured action fires and the warnings
are cleared.

**Usage:** `[target] [reason]` · **Role:** admin · **Rights:** restrict (caller and bot)

**Examples**

- `/warn` (as a reply) — add a warning with no reason.
- `/warn @dave spam` — warn with a reason.

**Edge cases**

- The limit and action are set by `/setwarnlimit` and `/setwarnaction`.
- A limit of `0` disables the auto-action.

### /unwarn

Remove a user's newest warning.

**Usage:** `[target]` · **Role:** admin · **Rights:** restrict (caller)

**Examples**

- `/unwarn` (as a reply) — remove the newest warning.

**Edge cases**

- With no warnings to remove, the bot says so rather than erroring.

### /warns

List your own warnings, or another user's (administrators only).

**Usage:** `[target]` · **Role:** anyone · **Rights:** none

**Examples**

- `/warns` — list your own warnings.
- `/warns @erin` — list another user's warnings (admin only).

**Edge cases**

- A plain member asking for another user's warnings is refused.
- At most 16 warnings are listed.

### /resetwarns

Clear all of a user's warnings.

**Usage:** `[target]` · **Role:** admin · **Rights:** restrict (caller)

**Examples**

- `/resetwarns` (as a reply) — clear the replied-to user's warnings.

**Edge cases**

- Requires a target; otherwise the bot replies with usage.

### /setwarnlimit

Set how many warnings trigger the action (`0` disables it).

**Usage:** `<n>` · **Role:** admin · **Rights:** restrict (caller)

**Examples**

- `/setwarnlimit 5` — fire the action at five warnings.
- `/setwarnlimit 0` — disable the auto-action.

**Edge cases**

- A non-numeric argument or a value above 1000 is rejected.

### /setwarnaction

Set the action taken at the warning limit.

**Usage:** `ban|mute|kick` · **Role:** admin · **Rights:** restrict (caller)

**Examples**

- `/setwarnaction kick` — kick at the limit.

**Edge cases**

- Any value other than `ban`, `mute`, or `kick` is rejected.

## Settings, welcome, rules

### /settings

Open the inline settings panel.

**Usage:** *(none)* · **Role:** admin · **Rights:** change_info (caller)

**Examples**

- `/settings` — post the panel with a Welcome toggle.

**Edge cases**

- The panel buttons are rejected if pressed by anyone but the opener.

### /setwelcome

Set the join welcome message. Placeholders: `{first}`, `{mention}`, `{chat}`,
`{count}`.

**Usage:** `<text>` · **Role:** admin · **Rights:** change_info (caller)

**Examples**

- `/setwelcome Welcome {first} to {chat}!` — set the template.

**Edge cases**

- An empty argument is rejected with usage.

### /welcome

Enable or disable the join welcome.

**Usage:** `on|off` · **Role:** admin · **Rights:** change_info (caller)

**Examples**

- `/welcome on` — enable the welcome.
- `/welcome off` — disable it.

**Edge cases**

- Any value other than `on` or `off` is rejected.

### /setrules

Set the chat rules.

**Usage:** `<text>` · **Role:** admin · **Rights:** change_info (caller)

**Examples**

- `/setrules Be nice.` — store the rules.

**Edge cases**

- An empty argument is rejected with usage.

### /rules

Show the chat rules.

**Usage:** *(none)* · **Role:** anyone · **Rights:** none

**Examples**

- `/rules` — print the stored rules.

**Edge cases**

- With no rules set, the bot says so.

### /setlog

Set or clear the moderation log channel.

**Usage:** `<chat_id|off>` · **Role:** owner · **Rights:** none

**Examples**

- `/setlog -1001234567890` — mirror actions to that channel.
- `/setlog off` — disable the log.

**Edge cases**

- A non-numeric argument that is not `off` is rejected with usage.

### /reload

Refresh the cached administrator list for the chat.

**Usage:** *(none)* · **Role:** admin · **Rights:** none

**Examples**

- `/reload` — drop the admin cache so the next command refetches it.

**Edge cases**

- Safe to run at any time; the cache also refreshes on any `chat_member` update.

## Automod

### /lock

Lock a message type (delete and log).

**Usage:** `<type> [type...]` · **Role:** admin · **Rights:** delete (caller and bot)

**Examples**

- `/lock links` — delete posted links.
- `/lock media stickers` — lock several types at once.

**Edge cases**

- An unknown type is rejected. Types: `links`, `media`, `stickers`, `forwards`,
  `polls`, `invites`, `bots`, `commands`.

### /unlock

Unlock a message type.

**Usage:** `<type> [type...]` · **Role:** admin · **Rights:** delete (caller and bot)

**Examples**

- `/unlock links` — stop deleting links.

**Edge cases**

- An unknown type is rejected.

### /blacklist

Manage the word and regex blacklist.

**Usage:** `add|del|list <word|/regex/>` · **Role:** admin · **Rights:** delete (caller and bot)

**Examples**

- `/blacklist add badword` — add a word.
- `/blacklist add /sp\w+/` — add a regex.
- `/blacklist del badword` — remove a pattern.
- `/blacklist list` — list up to 16 patterns.

**Edge cases**

- An invalid regex is rejected before it is stored.
- At most 16 patterns are kept.

### /antiflood

Set the flood threshold, or turn antiflood off.

**Usage:** `<messages> <seconds>|off` · **Role:** admin · **Rights:** restrict (caller and bot)

**Examples**

- `/antiflood 5 10` — mute after five messages in ten seconds.
- `/antiflood off` — disable antiflood.

**Edge cases**

- A zero or non-numeric threshold is rejected.

### /captcha

Enable or disable join verification.

**Usage:** `on|off` · **Role:** admin · **Rights:** restrict (caller and bot)

**Examples**

- `/captcha on` — challenge new members.
- `/captcha off` — disable the challenge.

**Edge cases**

- Any value other than `on` or `off` is rejected.

## Other

### /help

Show the commands the caller is allowed to use.

**Usage:** *(none)* · **Role:** anyone · **Rights:** none

**Examples**

- `/help` — list the available commands.

**Edge cases**

- The list is filtered by the caller's role and by whether the chat is a group.

### /start

Show the same text as `/help`. It is the only command besides `/help` that works
in a private chat.

**Usage:** *(none)* · **Role:** anyone · **Rights:** none

**Examples**

- `/start` — list the available commands.

**Edge cases**

- Works in private chats; group-only commands are hidden there.
