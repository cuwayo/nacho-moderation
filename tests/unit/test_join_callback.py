"""Tests for the join and callback-query handlers."""

from __future__ import annotations

from aiogram.types import Chat, ChatMemberLeft, ChatMemberMember, ChatMemberUpdated, User

from nacho_moderator.clock import FakeClock
from nacho_moderator.db import Database
from nacho_moderator.handlers import CallbackHandler, JoinHandler
from tests.conftest import ADMIN_ID, CHAT_ID, TARGET_ID, FakeBot
from tests.unit.test_dispatch import _app

_CHAT = Chat(id=CHAT_ID, type="supergroup", title="Testers")


def _join() -> ChatMemberUpdated:
    return ChatMemberUpdated(
        chat=_CHAT,
        from_user=User(id=TARGET_ID, is_bot=False, first_name="U"),
        date=1700000000,
        old_chat_member=ChatMemberLeft(user=User(id=TARGET_ID, is_bot=False, first_name="U")),
        new_chat_member=ChatMemberMember(user=User(id=TARGET_ID, is_bot=False, first_name="U")),
    )


class _FakeQuery:
    """A minimal CallbackQuery stand-in for the callback handler."""

    def __init__(self, data: str, from_id: int, message_id: int = 5) -> None:
        self.data = data
        self.from_user = User(id=from_id, is_bot=False, first_name="U")
        self.message = type("M", (), {"message_id": message_id})()

    async def answer(self, text: str | None = None) -> None:
        self.answered = text  # type: ignore[attr-defined]


async def test_join_invalidates_admin_cache(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    fake_bot.admins = []
    handler = JoinHandler(app)
    await handler.handle(_join())
    assert fake_bot.called("get_chat_administrators") == []  # invalidate only; no fetch


async def test_join_without_captcha_sends_welcome(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    await db.set_setting(CHAT_ID, "welcome_enabled", 1)
    await db.set_setting(CHAT_ID, "welcome_text", "Hi {first}")
    handler = JoinHandler(app)
    await handler.handle(_join())
    assert any("Hi U" in t for t in fake_bot.texts())


async def test_join_with_captcha_restricts(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    await db.set_setting(CHAT_ID, "captcha_enabled", 1)
    handler = JoinHandler(app)
    await handler.handle(_join())
    assert fake_bot.called("restrict_chat_member")
    assert any("confirm you are human" in t for t in fake_bot.texts())


async def test_leave_transition_does_nothing(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    handler = JoinHandler(app)
    event = ChatMemberUpdated(
        chat=_CHAT,
        from_user=User(id=TARGET_ID, is_bot=False, first_name="U"),
        date=1700000000,
        old_chat_member=ChatMemberMember(user=User(id=TARGET_ID, is_bot=False, first_name="U")),
        new_chat_member=ChatMemberLeft(user=User(id=TARGET_ID, is_bot=False, first_name="U")),
    )
    await handler.handle(event)
    assert fake_bot.called("send_message") == []


async def test_callback_wrong_user_not_owned(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    handler = CallbackHandler(app)
    query = _FakeQuery(f"set:{CHAT_ID}:{ADMIN_ID}:welcome", from_id=999)
    await handler.handle(query)
    assert query.answered == "This panel is not yours."  # type: ignore[attr-defined]
    assert fake_bot.called("edit_message_text") == []


async def test_callback_unknown_key(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    app = await _app(fake_bot, db, fake_clock)
    handler = CallbackHandler(app)
    query = _FakeQuery(f"set:{CHAT_ID}:{ADMIN_ID}:bogus", from_id=ADMIN_ID)
    await handler.handle(query)
    assert query.answered == "Unknown setting."  # type: ignore[attr-defined]


async def test_callback_toggles_welcome(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    handler = CallbackHandler(app)
    query = _FakeQuery(f"set:{CHAT_ID}:{ADMIN_ID}:welcome", from_id=ADMIN_ID)
    await handler.handle(query)
    assert (await db.get_settings(CHAT_ID)).welcome_enabled is True
    assert fake_bot.called("edit_message_text")
    assert query.answered == "Welcome enabled."  # type: ignore[attr-defined]


async def test_callback_captcha_verify(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    await app.captcha.begin(CHAT_ID, TARGET_ID, timeout_seconds=120)
    handler = CallbackHandler(app)
    query = _FakeQuery(f"cap:{CHAT_ID}:{TARGET_ID}:deadbeef", from_id=TARGET_ID)
    await handler.handle(query)
    assert query.answered == "Verified. Welcome!"  # type: ignore[attr-defined]
