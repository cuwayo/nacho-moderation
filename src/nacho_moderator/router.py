"""Command parsing, target resolution, permission gating, and dispatch.

The router turns an incoming :class:`~aiogram.types.Message` into a
:class:`~nacho_moderator.commands.context.CommandContext` and runs the matching
handler. Parsing mirrors the original bot:

* the text must start with ``/``; the name is lowercased and matched exactly;
* a ``/cmd@Bot`` suffix names another bot and is ignored unless it matches;
* the target token is consumed only for commands flagged ``TARGET`` and is an
  ``@username`` or an all-digit id;
* the replied-to message's sender always wins over an explicit target token.

Permissions are checked against the caller's resolved role and rights, and
against the bot's own rights in the chat.
"""

from __future__ import annotations

from dataclasses import dataclass

from aiogram.types import Message

from nacho_moderator.commands.registry import CommandSpec
from nacho_moderator.constants import TELEGRAM_MAX_COMMAND_NAME_LEN, Right, Role
from nacho_moderator.errors import BotLacksRightError, PermissionDeniedError
from nacho_moderator.logging_setup import get_logger
from nacho_moderator.models import TargetRef
from nacho_moderator.services.perms import Subject

_log = get_logger("router")

_DENIED = "You are not allowed to use that command."
_BOT_LACKS = "I lack the rights to do that in this chat."
_GROUP_ONLY = "That command is only available in groups."
_CANNOT_ACT = "I cannot act on the owner, an administrator, or myself."


@dataclass
class ParsedCommand:
    """The result of parsing a message's text as a command."""

    name: str
    args: str
    target: TargetRef | None
    addressed_to_other_bot: bool


def parse_command(text: str, want_target: bool, bot_username: str | None) -> ParsedCommand | None:
    """Parse a message's text as a bot command.

    Args:
        text: The message text.
        want_target: Whether the command consumes a leading target token.
        bot_username: The bot's own username (without ``@``), or ``None`` to
            accept any ``/cmd@Bot`` suffix.

    Returns:
        A :class:`ParsedCommand`, or ``None`` when the text is not a command.
    """
    if not text.startswith("/"):
        return None
    rest = text[1:]
    name_end = 0
    while name_end < len(rest) and not rest[name_end].isspace() and rest[name_end] != "@":
        name_end += 1
    name = rest[:name_end].lower()
    if not name or len(name) > TELEGRAM_MAX_COMMAND_NAME_LEN:
        return None

    tail = rest[name_end:]
    other_bot = False
    if tail.startswith("@"):
        at_end = 1
        while at_end < len(tail) and not tail[at_end].isspace():
            at_end += 1
        suffix = tail[1:at_end]
        if bot_username is not None and suffix.lower() != bot_username.lower():
            other_bot = True
        tail = tail[at_end:]

    args = tail.strip()
    target: TargetRef | None = None
    if want_target and args:
        token, _, remainder = args.partition(" ")
        parsed = _parse_target_token(token)
        if parsed is not None:
            target = parsed
            args = remainder.strip()
    return ParsedCommand(name=name, args=args, target=target, addressed_to_other_bot=other_bot)


def _parse_target_token(token: str) -> TargetRef | None:
    """Parse a single ``@username`` or all-digit target token."""
    if token.startswith("@"):
        if len(token) > 1:
            return TargetRef(username=token[1:])
        return None
    if token.isdigit():
        return TargetRef(user_id=int(token))
    return None


async def resolve_target(
    message: Message, target: TargetRef | None, admins: dict[int, Subject], db: object
) -> int | None:
    """Resolve a command's target to a user id.

    The replied-to message's sender wins; then a numeric id; then an
    ``@username`` resolved against the admin list and the users cache.
    """
    if message.reply_to_message is not None and message.reply_to_message.from_user is not None:
        return message.reply_to_message.from_user.id
    if target is None:
        return None
    if target.user_id is not None:
        return target.user_id
    assert target.username is not None
    lowered = target.username.lower()
    for subject in admins.values():
        if subject.username is not None and subject.username.lower() == lowered:
            return subject.user_id
    find = getattr(db, "find_user_by_username", None)
    if find is None:
        return None
    result: int | None = await find(target.username)
    return result


def check_permission(spec: CommandSpec, caller: Subject, bot_subject: Subject) -> None:
    """Raise when the caller or the bot may not run ``spec``.

    The bot's rights are checked first, then owner/sudo bypass the caller check,
    then the minimum role and the caller's specific right.
    """
    if spec.bot_right is not Right.NONE and not bot_subject.has(spec.bot_right):
        raise BotLacksRightError(_BOT_LACKS)
    if caller.role is Role.OWNER or caller.is_sudo:
        return
    if spec.min_role is Role.OWNER:
        raise PermissionDeniedError(_DENIED)
    if spec.min_role is Role.SUDO and not caller.is_sudo:
        raise PermissionDeniedError(_DENIED)
    if spec.min_role is Role.ADMIN and caller.role is not Role.ADMIN:
        raise PermissionDeniedError(_DENIED)
    if caller.role is Role.ADMIN and not caller.has(spec.user_right):
        raise PermissionDeniedError(_DENIED)


def target_is_protected(target_id: int, admins: dict[int, Subject], bot_id: int) -> bool:
    """Return whether a target may not be acted upon (owner/admin/self)."""
    if target_id == bot_id:
        return True
    subject = admins.get(target_id)
    return subject is not None and subject.role in (Role.OWNER, Role.ADMIN)
