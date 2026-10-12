"""The per-invocation command context.

A :class:`CommandContext` bundles everything a handler needs: the parsed
invocation, the resolved caller and target, the bot, the database, the clock,
and the services. It also provides the shared reply and moderation-log helpers
so every handler records actions and posts to the log channel the same way.
"""

from __future__ import annotations

import contextlib
from dataclasses import dataclass
from typing import TYPE_CHECKING

from aiogram import Bot
from aiogram.types import Message

from nacho_moderator.clock import Clock
from nacho_moderator.logging_setup import get_logger
from nacho_moderator.models import TargetRef, html_escape
from nacho_moderator.services.perms import Subject

_log = get_logger("cmd_ctx")

if TYPE_CHECKING:
    from nacho_moderator.db import Database
    from nacho_moderator.services.automod import AutomodService
    from nacho_moderator.services.settings import SettingsService


@dataclass
class Invocation:
    """A parsed command invocation."""

    name: str
    args: str = ""
    target: TargetRef | None = None
    duration_seconds: int | None = None
    reason: str = ""


@dataclass
class CommandContext:
    """Everything a command handler needs, plus shared reply helpers."""

    bot: Bot
    db: Database
    settings: SettingsService
    clock: Clock
    message: Message
    chat_id: int
    invocation: Invocation
    caller: Subject
    target_id: int | None = None
    target_label: str = ""
    bot_username: str | None = None
    is_group: bool = True
    #: Called by /reload to drop the cached admin list for this chat.
    invalidate_admins: object | None = None
    #: The automod service, so blacklist writes can drop its cache.
    automod: AutomodService | None = None
    log_chat_id: int = 0

    @property
    def text(self) -> str:
        """The full text of the invoking message."""
        return self.message.text or ""

    def now(self) -> int:
        """Return the current wall-clock time as a Unix timestamp."""
        return int(self.clock.now())

    async def reply(self, text: str) -> Message | None:
        """Send an HTML-escaped reply to the invoking chat."""
        return await self.bot.send_message(
            chat_id=self.chat_id,
            text=html_escape(text),
            reply_to_message_id=self.message.message_id,
        )

    async def reply_html(self, text: str) -> Message | None:
        """Send a reply whose text is already HTML markup."""
        return await self.bot.send_message(
            chat_id=self.chat_id,
            text=text,
            reply_to_message_id=self.message.message_id,
        )

    async def record_action(
        self,
        action: str,
        target_id: int,
        reason: str,
        created_at: int,
        expires_at: int | None,
        target_label: str = "",
    ) -> None:
        """Record a moderation action and mirror it to the log channel."""
        await self.db.record_action(
            chat_id=self.chat_id,
            user_id=target_id,
            admin_id=self.caller.user_id,
            action=action,
            reason=reason,
            created_at=created_at,
            expires_at=expires_at,
        )
        if not self.log_chat_id:
            return
        label = target_label or str(target_id)
        reason_text = reason or "-"
        line = (
            f"action={action} target={html_escape(label)} (id {target_id}) "
            f"admin={self.caller.user_id} reason={html_escape(reason_text)}"
        )
        if expires_at:
            line += f" until={expires_at}"
        with contextlib.suppress(Exception):
            # Mirroring to the log channel must never break the command itself.
            await self.bot.send_message(chat_id=self.log_chat_id, text=line)
        _log.debug("recorded action %s against %s", action, target_id)
