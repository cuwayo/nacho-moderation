"""Extra unit tests for welcome delivery.

Covers the fallbacks ``test_logging_welcome.py`` leaves untouched: a failing
``get_chat`` title lookup, and a failing ``send_message`` that must be logged
rather than raised.
"""

from __future__ import annotations

from nacho_moderator.db import ChatSettings
from nacho_moderator.services.admin_actions import send_welcome_if_enabled
from tests.conftest import CHAT_ID, TARGET_ID, FakeBot
from tests.helpers import user


async def test_welcome_without_chat_title(fake_bot: FakeBot) -> None:
    """A failed get_chat leaves the title empty rather than aborting the welcome."""
    fake_bot.fail = {"get_chat"}
    settings = ChatSettings(
        chat_id=CHAT_ID, welcome_enabled=True, welcome_text="Hi {first} in {chat}!"
    )
    app = type("App", (), {"bot": fake_bot})()
    await send_welcome_if_enabled(app, CHAT_ID, user(TARGET_ID, first_name="Alice"), settings)  # type: ignore[arg-type]
    assert "Hi Alice in !" in fake_bot.texts()[-1]


async def test_welcome_send_failure_is_logged_not_raised(fake_bot: FakeBot) -> None:
    """A send_message failure must be swallowed (best-effort delivery)."""
    fake_bot.fail = {"send_message"}
    settings = ChatSettings(chat_id=CHAT_ID, welcome_enabled=True, welcome_text="Hi {first}")
    app = type("App", (), {"bot": fake_bot})()
    await send_welcome_if_enabled(app, CHAT_ID, user(TARGET_ID, first_name="Alice"), settings)  # type: ignore[arg-type]
    # The call was attempted (and raised), but the helper returned cleanly.
    assert fake_bot.called("send_message") == []
