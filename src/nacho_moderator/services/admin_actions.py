"""Welcome delivery shared by the join handler and the captcha flow."""

from __future__ import annotations

from typing import TYPE_CHECKING

from aiogram.types import User

from nacho_moderator.logging_setup import get_logger
from nacho_moderator.services.welcome import render_welcome

if TYPE_CHECKING:
    from nacho_moderator.db import ChatSettings
    from nacho_moderator.handlers import App

_log = get_logger("welcome")


async def send_welcome_if_enabled(
    app: App, chat_id: int, user: User, settings: ChatSettings
) -> None:
    """Send the rendered welcome to ``chat_id`` when the chat has it enabled."""
    if not settings.welcome_enabled or not settings.welcome_text:
        return
    try:
        count = await app.bot.get_chat_member_count(chat_id)
    except Exception:
        count = 0
    title = ""
    try:
        chat = await app.bot.get_chat(chat_id)
        title = chat.title or ""
    except Exception:
        title = ""
    text = render_welcome(settings.welcome_text, user, title, count)
    try:
        await app.bot.send_message(chat_id=chat_id, text=text)
    except Exception as exc:
        _log.warning("failed to send welcome to %s: %s", chat_id, exc)
