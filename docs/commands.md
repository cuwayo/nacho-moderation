# Commands

Every command is declared once in `src/nacho_moderator/commands/registry.py`. The
`/help` text and the `setMyCommands` menu are both derived from that table, so a
command cannot exist in one place but not another.

Target resolution order: the replied-to message's sender, then a numeric user id,
then an `@username` (resolved against the chat's administrators and the users
cache). A duration suffix is `30s`, `10m`, `2h`, `7d`, or `1w`; without one the
action is permanent. Trailing free text is the reason.

## Moderation

| Command | Usage | Role | Description |
| --- | --- | --- | --- |
| `/ban` | `[target] [duration] [reason]` | admin | Ban a user; a duration makes it temporary. |
| `/unban` | `[target]` | admin | Lift a ban. |
| `/kick` | `[target] [reason]` | admin | Remove a user so they can rejoin. |
| `/mute` | `[target] [duration] [reason]` | admin | Restrict a user from sending. |
| `/unmute` | `[target]` | admin | Restore a user's permissions. |
| `/promote` | `[target] [title]` | admin | Grant limited moderator rights (delete, restrict, pin) with an optional title. |
| `/demote` | `[target]` | admin | Remove administrator rights. |
| `/del` | *(reply)* | admin | Delete the replied-to message and the command. |
| `/pin` | *(reply)* | admin | Pin the replied-to message. |
| `/unpin` | | admin | Unpin the last pinned message. |
| `/purge` | *(reply)* | admin | Delete from the replied message up to the command, in batches of 100. |

## Warnings

| Command | Usage | Role | Description |
| --- | --- | --- | --- |
| `/warn` | `[target] [reason]` | admin | Add a warning; auto-act at the limit. |
| `/unwarn` | `[target]` | admin | Remove the newest warning. |
| `/warns` | `[target]` | anyone | List your warnings, or a user's (admins). |
| `/resetwarns` | `[target]` | admin | Clear all warnings. |
| `/setwarnlimit` | `<n>` | admin | Warnings before the action fires (0 disables). |
| `/setwarnaction` | `ban\|mute\|kick` | admin | Action taken at the limit. |

## Settings, welcome, rules

| Command | Usage | Role | Description |
| --- | --- | --- | --- |
| `/settings` | | admin | Open the inline settings panel. |
| `/setwelcome` | `<text>` | admin | Set the join welcome (`{first}` `{mention}` `{chat}` `{count}`). |
| `/welcome` | `on\|off` | admin | Enable or disable the join welcome. |
| `/setrules` | `<text>` | admin | Set the chat rules. |
| `/rules` | | anyone | Show the chat rules. |
| `/setlog` | `<chat_id\|off>` | owner | Set or clear the moderation log channel. |
| `/reload` | | admin | Refresh the cached administrator list. |

## Automod

| Command | Usage | Role | Description |
| --- | --- | --- | --- |
| `/lock` | `<type> [type...]` | admin | Lock a message type (delete + log). |
| `/unlock` | `<type> [type...]` | admin | Unlock a message type. |
| `/blacklist` | `add\|del\|list <word\|/regex/>` | admin | Manage the word/regex blacklist. |
| `/antiflood` | `<messages> <seconds>\|off` | admin | Set the flood threshold. |
| `/captcha` | `on\|off` | admin | Enable or disable join verification. |

Lock types: `links`, `media`, `stickers`, `forwards`, `polls`, `invites`, `bots`,
`commands`.

## Other

| Command | Role | Description |
| --- | --- | --- |
| `/help` | anyone | Show available commands. |
| `/start` | anyone | Show available commands (the only command that works in private chats besides `/help`). |

## Promoting and demoting

`/promote` grants only a limited, explicitly safe set of rights: delete messages,
restrict members, and pin messages. It never grants `can_promote_members`,
`can_change_info`, or `can_manage_chat`. The caller needs `can_promote_members`,
and so does the bot. An optional custom title is trimmed to Telegram's limit of
16 characters and HTML-escaped when echoed back.

`/demote` clears every administrator right. It refuses the chat owner and the bot
itself, and Telegram rejects an administrator the bot did not promote — that
rejection is reported with a clear message and never retried.

## Edge cases

- The owner, other administrators, and the bot itself cannot be acted upon.
- `/demote` is the one exception: it may act on an administrator (but still never
  on the owner or the bot), because removing admin rights is its purpose.
- A non-administrator cannot run an admin command; the bot replies clearly when
  it lacks the required right.
- A temporary ban or mute survives a restart and is lifted by the sweeper.
- The captcha button is rejected if pressed by anyone other than the challenged
  member, and the member is kicked if the deadline passes.
- `chat_member` updates (used for join handling) are only delivered while the bot
  is an administrator in the chat.
