"""Tests for logging redaction and the welcome-delivery helper."""

from __future__ import annotations

from nacho_moderator.db import ChatSettings
from nacho_moderator.logging_setup import redact
from nacho_moderator.services.admin_actions import send_welcome_if_enabled
from tests.conftest import CHAT_ID, TARGET_ID, FakeBot
from tests.helpers import user


def test_redact_removes_token_from_url() -> None:
    text = "POST https://api.telegram.org/bot123456:SECRETTOKEN/getMe failed"
    redacted = redact(text)
    assert "SECRETTOKEN" not in redacted
    assert "bot<redacted>" in redacted


def test_redact_bare_token() -> None:
    assert "SECRETTOKEN" not in redact("token bot123456:SECRETTOKEN here")


def test_redact_leaves_plain_text() -> None:
    assert redact("nothing to see") == "nothing to see"


async def test_welcome_skipped_when_disabled(fake_bot: FakeBot) -> None:
    settings = ChatSettings(chat_id=CHAT_ID, welcome_enabled=False, welcome_text="Hi")
    app = type("App", (), {"bot": fake_bot})()
    await send_welcome_if_enabled(app, CHAT_ID, user(TARGET_ID), settings)  # type: ignore[arg-type]
    assert fake_bot.called("send_message") == []


async def test_welcome_sent_when_enabled(fake_bot: FakeBot) -> None:
    settings = ChatSettings(
        chat_id=CHAT_ID, welcome_enabled=True, welcome_text="Welcome {first} to {chat} ({count})"
    )
    app = type("App", (), {"bot": fake_bot})()
    await send_welcome_if_enabled(app, CHAT_ID, user(TARGET_ID, first_name="Alice"), settings)  # type: ignore[arg-type]
    sends = fake_bot.called("send_message")
    assert sends
    assert "Welcome Alice to Testers (42)" in sends[0].kwargs["text"]


async def test_welcome_survives_missing_count(fake_bot: FakeBot) -> None:
    fake_bot.fail = {"get_chat_member_count"}
    settings = ChatSettings(chat_id=CHAT_ID, welcome_enabled=True, welcome_text="Hi {count}")
    app = type("App", (), {"bot": fake_bot})()
    await send_welcome_if_enabled(app, CHAT_ID, user(TARGET_ID), settings)  # type: ignore[arg-type]
    assert "Hi 0" in fake_bot.texts()[-1]
