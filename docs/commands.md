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

## Edge cases

- The owner, other administrators, and the bot itself cannot be acted upon.
- A non-administrator cannot run an admin command; the bot replies clearly when
  it lacks the required right.
- A temporary ban or mute survives a restart and is lifted by the sweeper.
- The captcha button is rejected if pressed by anyone other than the challenged
  member, and the member is kicked if the deadline passes.
- `chat_member` updates (used for join handling) are only delivered while the bot
  is an administrator in the chat.
