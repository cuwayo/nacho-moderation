"""Periodic lifter for expired temporary bans and mutes.

A background task runs every 30 seconds, asks the database for due actions, and
lifts each one: a ``ban`` becomes ``unbanChatMember(only_if_banned=True)``, a
``mute`` restores the common group permissions, and the row is marked lifted so
it is never applied twice. Because ``expires_at`` is an absolute wall-clock
timestamp, this survives a process restart.
"""

from __future__ import annotations

from aiogram import Bot
from aiogram.types import ChatPermissions

from nacho_moderator.clock import Clock
from nacho_moderator.constants import (
    CAPTCHA_SWEEP_INTERVAL_SECONDS,
    EXPIRY_INTERVAL_SECONDS,
)
from nacho_moderator.db import Database
from nacho_moderator.logging_setup import get_logger
from nacho_moderator.services.captcha import CaptchaService
from nacho_moderator.services.periodic import PeriodicTask

_log = get_logger("expiry")

_RESTORED = ChatPermissions(
    can_send_messages=True,
    can_send_audios=True,
    can_send_documents=True,
    can_send_photos=True,
    can_send_videos=True,
    can_send_video_notes=True,
    can_send_voice_notes=True,
    can_send_polls=True,
    can_send_other_messages=True,
    can_add_web_page_previews=True,
    can_invite_users=True,
)


class ExpirySweeper(PeriodicTask):
    """Lifts expired temp bans and mutes on a fixed interval."""

    def __init__(
        self,
        db: Database,
        clock: Clock,
        bot: Bot,
        interval: float = EXPIRY_INTERVAL_SECONDS,
        batch: int = 32,
    ) -> None:
        super().__init__(interval, "expiry")
        self._db = db
        self._clock = clock
        self._bot = bot
        self._batch = batch

    @property
    def clock(self) -> Clock:
        """Return the injected clock (shared with the handlers)."""
        return self._clock

    async def sweep_once(self) -> int:
        """Lift every currently-due action; return how many were processed."""
        due = await self._db.due_actions(int(self._clock.now()), self._batch)
        for row in due:
            await self._lift(row.chat_id, row.user_id, row.action)
            await self._db.lift_action(row.id)
        return len(due)

    async def _tick(self) -> None:
        await self.sweep_once()

    async def _lift(self, chat_id: int, user_id: int, action: str) -> None:
        """Apply the inverse of one expiring action."""
        try:
            if action == "ban":
                await self._bot.unban_chat_member(
                    chat_id=chat_id, user_id=user_id, only_if_banned=True
                )
            elif action == "mute":
                await self._bot.restrict_chat_member(
                    chat_id=chat_id,
                    user_id=user_id,
                    permissions=_RESTORED,
                    use_independent_chat_permissions=True,
                )
        except Exception as exc:
            _log.warning("failed to lift %s for %s in %s: %s", action, user_id, chat_id, exc)


class CaptchaSweeper(PeriodicTask):
    """Kicks members whose captcha deadline has passed, on a fixed interval."""

    def __init__(
        self,
        captcha: CaptchaService,
        interval: float = CAPTCHA_SWEEP_INTERVAL_SECONDS,
    ) -> None:
        super().__init__(interval, "captcha")
        self._captcha = captcha

    async def _tick(self) -> None:
        await self._captcha.sweep_expired()
