"""The command registry: one table that drives /help and setMyCommands.

Every command is declared once as a :class:`CommandSpec`. The ``/help`` text, the
two ``setMyCommands`` payloads, and the router's dispatch table are all derived
from this list, so a command cannot exist in one place but not another.
"""

from __future__ import annotations

from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from typing import TYPE_CHECKING

from nacho_moderator.constants import CommandFlag, Right, Role

if TYPE_CHECKING:
    from nacho_moderator.commands.context import CommandContext

Handler = Callable[["CommandContext"], Awaitable[None]]


@dataclass(frozen=True)
class CommandSpec:
    """The static description of one bot command."""

    id: str
    name: str
    min_role: Role
    user_right: Right
    bot_right: Right
    flags: CommandFlag
    usage: str
    brief: str
    handler: Handler

    def visible_to(self, role: Role, *, is_group: bool) -> bool:
        """Return whether the command is listed to a caller of ``role``."""
        if CommandFlag.HIDDEN in self.flags:
            return False
        if CommandFlag.GROUP_ONLY in self.flags and not is_group:
            return False
        if role in (Role.OWNER, Role.SUDO):
            return True
        return role >= self.min_role


def _build_registry() -> list[CommandSpec]:
    # Imported here to avoid a circular import at module load time.
    from nacho_moderator.commands import (
        automod_cmds,
        help_cmd,
        moderation,
        promote,
        settings_cmds,
        warns,
    )

    return [
        CommandSpec(
            "help",
            "help",
            Role.ANYONE,
            Right.NONE,
            Right.NONE,
            CommandFlag.NONE,
            "",
            "Show available commands",
            help_cmd.cmd_help,
        ),
        CommandSpec(
            "start",
            "start",
            Role.ANYONE,
            Right.NONE,
            Right.NONE,
            CommandFlag.NONE,
            "",
            "Show available commands",
            help_cmd.cmd_start,
        ),
        CommandSpec(
            "ban",
            "ban",
            Role.ADMIN,
            Right.RESTRICT,
            Right.RESTRICT,
            CommandFlag.TARGET | CommandFlag.DURATION | CommandFlag.REASON | CommandFlag.GROUP_ONLY,
            "[target] [duration] [reason]",
            "Ban a user from the chat",
            moderation.cmd_ban,
        ),
        CommandSpec(
            "unban",
            "unban",
            Role.ADMIN,
            Right.RESTRICT,
            Right.RESTRICT,
            CommandFlag.TARGET | CommandFlag.GROUP_ONLY,
            "[target]",
            "Lift a ban",
            moderation.cmd_unban,
        ),
        CommandSpec(
            "kick",
            "kick",
            Role.ADMIN,
            Right.RESTRICT,
            Right.RESTRICT,
            CommandFlag.TARGET | CommandFlag.REASON | CommandFlag.GROUP_ONLY,
            "[target] [reason]",
            "Remove a user so they can rejoin",
            moderation.cmd_kick,
        ),
        CommandSpec(
            "mute",
            "mute",
            Role.ADMIN,
            Right.RESTRICT,
            Right.RESTRICT,
            CommandFlag.TARGET | CommandFlag.DURATION | CommandFlag.REASON | CommandFlag.GROUP_ONLY,
            "[target] [duration] [reason]",
            "Restrict a user from sending messages",
            moderation.cmd_mute,
        ),
        CommandSpec(
            "unmute",
            "unmute",
            Role.ADMIN,
            Right.RESTRICT,
            Right.RESTRICT,
            CommandFlag.TARGET | CommandFlag.GROUP_ONLY,
            "[target]",
            "Restore a user's permissions",
            moderation.cmd_unmute,
        ),
        CommandSpec(
            "promote",
            "promote",
            Role.ADMIN,
            Right.PROMOTE,
            Right.PROMOTE,
            CommandFlag.TARGET | CommandFlag.REASON | CommandFlag.GROUP_ONLY,
            "[target] [title]",
            "Grant limited moderator rights",
            promote.cmd_promote,
        ),
        CommandSpec(
            "demote",
            "demote",
            Role.ADMIN,
            Right.PROMOTE,
            Right.PROMOTE,
            CommandFlag.TARGET | CommandFlag.ADMIN_TARGET | CommandFlag.GROUP_ONLY,
            "[target]",
            "Remove administrator rights",
            promote.cmd_demote,
        ),
        CommandSpec(
            "del",
            "del",
            Role.ADMIN,
            Right.DELETE,
            Right.DELETE,
            CommandFlag.REPLY_REQUIRED | CommandFlag.GROUP_ONLY,
            "",
            "Delete the replied-to message and the command",
            moderation.cmd_del,
        ),
        CommandSpec(
            "pin",
            "pin",
            Role.ADMIN,
            Right.PIN,
            Right.PIN,
            CommandFlag.REPLY_REQUIRED | CommandFlag.GROUP_ONLY,
            "",
            "Pin the replied-to message",
            moderation.cmd_pin,
        ),
        CommandSpec(
            "unpin",
            "unpin",
            Role.ADMIN,
            Right.PIN,
            Right.PIN,
            CommandFlag.GROUP_ONLY,
            "",
            "Unpin the last pinned message",
            moderation.cmd_unpin,
        ),
        CommandSpec(
            "purge",
            "purge",
            Role.ADMIN,
            Right.DELETE,
            Right.DELETE,
            CommandFlag.REPLY_REQUIRED | CommandFlag.GROUP_ONLY,
            "",
            "Delete messages from the replied message to now",
            moderation.cmd_purge,
        ),
        CommandSpec(
            "warn",
            "warn",
            Role.ADMIN,
            Right.RESTRICT,
            Right.RESTRICT,
            CommandFlag.TARGET | CommandFlag.REASON | CommandFlag.GROUP_ONLY,
            "[target] [reason]",
            "Warn a user; auto-act at the limit",
            warns.cmd_warn,
        ),
        CommandSpec(
            "unwarn",
            "unwarn",
            Role.ADMIN,
            Right.RESTRICT,
            Right.NONE,
            CommandFlag.TARGET | CommandFlag.GROUP_ONLY,
            "[target]",
            "Remove a user's newest warning",
            warns.cmd_unwarn,
        ),
        CommandSpec(
            "warns",
            "warns",
            Role.ANYONE,
            Right.NONE,
            Right.NONE,
            CommandFlag.TARGET | CommandFlag.GROUP_ONLY,
            "[target]",
            "List your warnings, or a user's (admins)",
            warns.cmd_warns,
        ),
        CommandSpec(
            "resetwarns",
            "resetwarns",
            Role.ADMIN,
            Right.RESTRICT,
            Right.NONE,
            CommandFlag.TARGET | CommandFlag.GROUP_ONLY,
            "[target]",
            "Clear all of a user's warnings",
            warns.cmd_resetwarns,
        ),
        CommandSpec(
            "setwarnlimit",
            "setwarnlimit",
            Role.ADMIN,
            Right.RESTRICT,
            Right.NONE,
            CommandFlag.GROUP_ONLY,
            "<n>",
            "Set how many warnings trigger the action",
            warns.cmd_setwarnlimit,
        ),
        CommandSpec(
            "setwarnaction",
            "setwarnaction",
            Role.ADMIN,
            Right.RESTRICT,
            Right.NONE,
            CommandFlag.GROUP_ONLY,
            "ban|mute|kick",
            "Set the action taken at the warning limit",
            warns.cmd_setwarnaction,
        ),
        CommandSpec(
            "setlog",
            "setlog",
            Role.OWNER,
            Right.NONE,
            Right.NONE,
            CommandFlag.GROUP_ONLY,
            "<chat_id|off>",
            "Set or clear the moderation log channel",
            settings_cmds.cmd_setlog,
        ),
        CommandSpec(
            "reload",
            "reload",
            Role.ADMIN,
            Right.NONE,
            Right.NONE,
            CommandFlag.GROUP_ONLY,
            "",
            "Refresh the cached administrator list",
            settings_cmds.cmd_reload,
        ),
        CommandSpec(
            "settings",
            "settings",
            Role.ADMIN,
            Right.CHANGE_INFO,
            Right.NONE,
            CommandFlag.GROUP_ONLY,
            "",
            "Open the inline settings panel",
            settings_cmds.cmd_settings,
        ),
        CommandSpec(
            "setwelcome",
            "setwelcome",
            Role.ADMIN,
            Right.CHANGE_INFO,
            Right.NONE,
            CommandFlag.GROUP_ONLY,
            "<text>",
            "Set the join welcome message",
            settings_cmds.cmd_setwelcome,
        ),
        CommandSpec(
            "welcome",
            "welcome",
            Role.ADMIN,
            Right.CHANGE_INFO,
            Right.NONE,
            CommandFlag.GROUP_ONLY,
            "on|off",
            "Enable or disable the join welcome",
            settings_cmds.cmd_welcome,
        ),
        CommandSpec(
            "setrules",
            "setrules",
            Role.ADMIN,
            Right.CHANGE_INFO,
            Right.NONE,
            CommandFlag.GROUP_ONLY,
            "<text>",
            "Set the chat rules",
            settings_cmds.cmd_setrules,
        ),
        CommandSpec(
            "rules",
            "rules",
            Role.ANYONE,
            Right.NONE,
            Right.NONE,
            CommandFlag.GROUP_ONLY,
            "",
            "Show the chat rules",
            settings_cmds.cmd_rules,
        ),
        CommandSpec(
            "lock",
            "lock",
            Role.ADMIN,
            Right.DELETE,
            Right.DELETE,
            CommandFlag.GROUP_ONLY,
            "<type> [type...]",
            "Lock a message type (delete + log)",
            automod_cmds.cmd_lock,
        ),
        CommandSpec(
            "unlock",
            "unlock",
            Role.ADMIN,
            Right.DELETE,
            Right.DELETE,
            CommandFlag.GROUP_ONLY,
            "<type> [type...]",
            "Unlock a message type",
            automod_cmds.cmd_unlock,
        ),
        CommandSpec(
            "blacklist",
            "blacklist",
            Role.ADMIN,
            Right.DELETE,
            Right.DELETE,
            CommandFlag.GROUP_ONLY,
            "add|del|list <word|/regex/>",
            "Manage the chat word/regex blacklist",
            automod_cmds.cmd_blacklist,
        ),
        CommandSpec(
            "antiflood",
            "antiflood",
            Role.ADMIN,
            Right.RESTRICT,
            Right.RESTRICT,
            CommandFlag.GROUP_ONLY,
            "<messages> <seconds>|off",
            "Set the flood threshold, or turn it off",
            automod_cmds.cmd_antiflood,
        ),
        CommandSpec(
            "captcha",
            "captcha",
            Role.ADMIN,
            Right.RESTRICT,
            Right.RESTRICT,
            CommandFlag.GROUP_ONLY,
            "on|off",
            "Enable or disable join verification",
            automod_cmds.cmd_captcha,
        ),
    ]


COMMANDS: list[CommandSpec] = _build_registry()
_BY_NAME: dict[str, CommandSpec] = {c.name: c for c in COMMANDS}


def lookup(name: str) -> CommandSpec | None:
    """Look up a command by its lowercased name (no leading slash)."""
    return _BY_NAME.get(name)


def render_help(role: Role, *, is_group: bool) -> str:
    """Render the ``/help`` text for a caller of the given role."""
    lines = ["Available commands:"]
    for spec in COMMANDS:
        if spec.visible_to(role, is_group=is_group):
            lines.append(f"/{spec.name} - {spec.brief}")
    return "\n".join(lines)
