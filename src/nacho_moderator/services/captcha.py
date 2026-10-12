"""Join verification (captcha).

On a join transition the bot restricts the member, sends an inline "I'm human"
button, and records a pending row with a deadline. A correct press lifts the
restriction and delivers the welcome; a wrong press is rejected; a deadline that
passes kicks the member (ban then unban) and cleans up the prompt.
"""

from __future__ import annotations

from dataclasses import dataclass

from aiogram import Bot
from aiogram.types import ChatPermissions, InlineKeyboardButton, InlineKeyboardMarkup

from nacho_moderator.clock import Clock, RandomSource
from nacho_moderator.constants import DEFAULT_CAPTCHA_TIMEOUT
from nacho_moderator.db import Database
from nacho_moderator.logging_setup import get_logger

_log = get_logger("captcha")

CAPTCHA_CB_PREFIX = "cap:"
CAPTCHA_PROMPT = "Welcome! Please confirm you are human to stay in the group."
CAPTCHA_BUTTON = "I'm human"

# The common group permissions, granted on success and revoked while pending.
_GRANTED = ChatPermissions(
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

_REVOKED = ChatPermissions(
    can_send_messages=False,
    can_send_audios=False,
    can_send_documents=False,
    can_send_photos=False,
    can_send_videos=False,
    can_send_video_notes=False,
    can_send_voice_notes=False,
    can_send_polls=False,
    can_send_other_messages=False,
    can_add_web_page_previews=False,
    can_change_info=False,
    can_invite_users=False,
    can_pin_messages=False,
)


@dataclass(frozen=True)
class CaptchaCallback:
    """A parsed ``cap:<chat_id>:<user_id>:<nonce>`` callback payload."""

    chat_id: int
    user_id: int
    nonce: str


def parse_captcha_callback(data: str) -> CaptchaCallback | None:
    """Parse a captcha callback payload, or return ``None`` if it is not one."""
    if not data.startswith(CAPTCHA_CB_PREFIX):
        return None
    parts = data.split(":")
    if len(parts) != 4 or parts[3] == "":
        return None
    try:
        chat_id = int(parts[1])
        user_id = int(parts[2])
    except ValueError:
        return None
    return CaptchaCallback(chat_id=chat_id, user_id=user_id, nonce=parts[3])


class CaptchaService:
    """Issue, verify, and expire join challenges."""

    def __init__(self, db: Database, clock: Clock, random: RandomSource, bot: Bot) -> None:
        self._db = db
        self._clock = clock
        self._random = random
        self._bot = bot

    async def begin(
        self, chat_id: int, user_id: int, timeout_seconds: int = DEFAULT_CAPTCHA_TIMEOUT
    ) -> None:
        """Restrict a new member and send the challenge prompt."""
        await self._bot.restrict_chat_member(
            chat_id=chat_id,
            user_id=user_id,
            permissions=_REVOKED,
            use_independent_chat_permissions=True,
        )
        nonce = self._random.token_hex(4)
        markup = InlineKeyboardMarkup(
            inline_keyboard=[
                [
                    InlineKeyboardButton(
                        text=CAPTCHA_BUTTON,
                        callback_data=f"{CAPTCHA_CB_PREFIX}{chat_id}:{user_id}:{nonce}",
                    )
                ]
            ]
        )
        prompt = await self._bot.send_message(
            chat_id=chat_id, text=CAPTCHA_PROMPT, reply_markup=markup
        )
        deadline = int(self._clock.now()) + timeout_seconds
        await self._db.captcha_put(chat_id, user_id, prompt.message_id, nonce, deadline)

    async def verify(self, callback: CaptchaCallback, from_user_id: int) -> str:
        """Handle a captcha button press.

        Args:
            callback: The parsed callback payload.
            from_user_id: The id of the user who pressed the button.

        Returns:
            The answer text to show the presser.
        """
        if from_user_id != callback.user_id:
            return "This challenge is not yours."
        row = await self._db.captcha_get(callback.chat_id, callback.user_id)
        if row is None or row.nonce != callback.nonce:
            return "This challenge has expired."
        await self._bot.restrict_chat_member(
            chat_id=callback.chat_id,
            user_id=callback.user_id,
            permissions=_GRANTED,
            use_independent_chat_permissions=True,
        )
        try:
            await self._bot.delete_message(chat_id=callback.chat_id, message_id=row.message_id)
        except Exception:
            _log.debug("captcha prompt already deleted for %s", callback.user_id)
        await self._db.captcha_delete(callback.chat_id, callback.user_id)
        return "Verified. Welcome!"

    async def sweep_expired(self, limit: int = 16) -> int:
        """Kick members whose challenge deadline has passed.

        Returns:
            The number of members kicked.
        """
        due = await self._db.captcha_due(int(self._clock.now()), limit)
        for row in due:
            await self._kick(row.chat_id, row.user_id, row.message_id)
        if due:
            _log.info("kicked %d member(s) that failed the captcha", len(due))
        return len(due)

    async def _kick(self, chat_id: int, user_id: int, message_id: int) -> None:
        """Ban then unban a member (a kick), and remove the prompt row."""
        await self._bot.ban_chat_member(chat_id=chat_id, user_id=user_id)
        await self._bot.unban_chat_member(chat_id=chat_id, user_id=user_id, only_if_banned=True)
        try:
            await self._bot.delete_message(chat_id=chat_id, message_id=message_id)
        except Exception:
            _log.debug("captcha prompt already deleted for %s", user_id)
        await self._db.captcha_delete(chat_id, user_id)
