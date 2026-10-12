"""Publish the bot's command menu via ``setMyCommands``.

Two scopes are registered: ``all_group_chats`` (non-admin, non-hidden commands)
and ``all_chat_administrators`` (every non-hidden command). Because the
administrator scope is more specific, Telegram shows it to admins, so it must
carry the full set.
"""

from __future__ import annotations

from aiogram import Bot
from aiogram.types import (
    BotCommand,
    BotCommandScopeAllChatAdministrators,
    BotCommandScopeAllGroupChats,
)

from nacho_moderator.commands.registry import COMMANDS
from nacho_moderator.constants import Role
from nacho_moderator.logging_setup import get_logger

_log = get_logger("publish")


def _bot_commands(*, admin: bool) -> list[BotCommand]:
    """Build the ``BotCommand`` list for one scope."""
    result: list[BotCommand] = []
    for spec in COMMANDS:
        if not spec.visible_to(Role.OWNER, is_group=True):
            continue
        if not admin and spec.min_role is not Role.ANYONE:
            continue
        result.append(BotCommand(command=spec.name, description=spec.brief))
    return result


async def publish_commands(bot: Bot) -> None:
    """Register the command menu for both scopes (best effort)."""
    try:
        await bot.set_my_commands(_bot_commands(admin=False), scope=BotCommandScopeAllGroupChats())
        await bot.set_my_commands(
            _bot_commands(admin=True), scope=BotCommandScopeAllChatAdministrators()
        )
    except Exception as exc:
        _log.warning("setMyCommands failed: %s", exc)
