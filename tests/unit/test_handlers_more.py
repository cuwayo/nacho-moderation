"""Extra unit tests for ``handlers.py``.

These complement ``test_dispatch.py`` and ``test_join_callback.py`` and target
the branches those files leave uncovered: the latency and context middlewares,
the non-command / bot-sender / unknown-command dispatch paths, ``_target_label``,
the callback edge cases, and ``build_dispatcher``'s registrations and lifecycle
hooks. All objects are real aiogram models bound to a :class:`FakeBot`, so the
tests exercise our wiring, not the library.
"""

from __future__ import annotations

from typing import Any

import pytest
from aiogram import Dispatcher
from aiogram.types import (
    CallbackQuery,
    Chat,
    Message,
    MessageEntity,
    TelegramObject,
    User,
)

from nacho_moderator.clock import FakeClock
from nacho_moderator.db import Database
from nacho_moderator.handlers import (
    CallbackHandler,
    ContextMiddleware,
    LatencyMiddleware,
    MessageHandler,
    _target_label,
    build_dispatcher,
)
from nacho_moderator.metrics import Metrics
from nacho_moderator.services.automod import Verdict
from tests.conftest import ADMIN_ID, BOT_ID, CHAT_ID, TARGET_ID, FakeBot, fake_admin
from tests.helpers import FIXED_DATE
from tests.unit.test_dispatch import _app, _message

_CHAT = Chat(id=CHAT_ID, type="supergroup", title="Testers")
_PRIVATE_CHAT = Chat(id=CHAT_ID, type="private")


def _group_message(
    text: str | None,
    *,
    user_id: int = TARGET_ID,
    message_id: int = 1,
    is_bot: bool = False,
    reply_to: Message | None = None,
    new_chat_members: list[User] | None = None,
) -> Message:
    """Build a supergroup ``Message`` for the middleware/handler tests."""
    return Message(
        message_id=message_id,
        date=FIXED_DATE,
        chat=_CHAT,
        from_user=User(id=user_id, is_bot=is_bot, first_name="User", username="u"),
        text=text,
        reply_to_message=reply_to,
        new_chat_members=new_chat_members,
    )


def _user(user_id: int) -> User:
    """Build a minimal non-bot ``User``."""
    return User(id=user_id, is_bot=False, first_name="U")


# --- LatencyMiddleware -----------------------------------------------------


async def test_latency_middleware_records_duration(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """``LatencyMiddleware.__call__`` times the handler and records the sample."""
    metrics = Metrics()
    middleware = LatencyMiddleware(metrics, fake_clock)
    ran: list[str] = []

    async def next_handler(event: Any, data: Any) -> str:
        fake_clock.advance(0.25)
        ran.append("yes")
        return "done"

    result = await middleware(next_handler, _group_message("hi"), {})
    assert result == "done"
    assert ran == ["yes"]
    assert metrics.snapshot().latency_count == 1


# --- ContextMiddleware early returns ---------------------------------------


async def test_context_middleware_passes_non_message_through(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """A non-Message update skips context resolution entirely."""
    app = await _app(fake_bot, db, fake_clock)
    middleware = ContextMiddleware(app)
    reached: list[bool] = []

    async def next_handler(event: Any, data: Any) -> None:
        reached.append(True)

    await middleware(next_handler, TelegramObject(), {})
    assert reached == [True]


async def test_context_middleware_passes_senderless_message(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """A Message with no ``from_user`` bypasses context resolution."""
    app = await _app(fake_bot, db, fake_clock)
    middleware = ContextMiddleware(app)
    reached: list[bool] = []

    async def next_handler(event: Any, data: Any) -> None:
        reached.append(True)

    msg = Message(message_id=1, date=FIXED_DATE, chat=_CHAT, text="hi")
    await middleware(next_handler, msg, {})
    assert reached == [True]


async def test_context_middleware_passes_private_chat(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """A message in a private chat is not run through automod."""
    app = await _app(fake_bot, db, fake_clock)
    middleware = ContextMiddleware(app)
    reached: list[bool] = []

    async def next_handler(event: Any, data: Any) -> None:
        reached.append(True)

    msg = Message(
        message_id=1,
        date=FIXED_DATE,
        chat=_PRIVATE_CHAT,
        from_user=_user(TARGET_ID),
        text="hi",
    )
    await middleware(next_handler, msg, {})
    assert reached == [True]


async def test_context_middleware_lookup_failure_passes_through(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """When the settings/admin lookup raises, the message still reaches the handler."""
    app = await _app(fake_bot, db, fake_clock)
    fake_bot.fail = {"get_chat_administrators"}
    middleware = ContextMiddleware(app)
    reached: list[bool] = []

    async def next_handler(event: Any, data: Any) -> None:
        reached.append(True)

    await middleware(next_handler, _group_message("hello"), {})
    assert reached == [True]


# --- ContextMiddleware automod verdicts ------------------------------------


async def test_middleware_delete_failure_is_swallowed(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """A failed delete in the automod DELETE branch is ignored, not raised."""
    app = await _app(fake_bot, db, fake_clock)
    fake_bot.admins = [fake_admin(BOT_ID)]
    await db.set_setting(CHAT_ID, "locks", 1 << 0)  # lock links
    fake_bot.fail = {"delete_message"}
    middleware = ContextMiddleware(app)
    reached: list[bool] = []

    async def next_handler(event: Any, data: Any) -> None:
        reached.append(True)

    msg = _message(
        "https://example.com",
        user_id=TARGET_ID,
        entities=[MessageEntity(type="url", offset=0, length=19)],
    )
    await middleware(next_handler, msg, {})
    assert reached == []  # propagation stopped even though the delete failed


async def test_middleware_mutes_flooding_member(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """The MUTE verdict restricts the member and removes the burst."""
    app = await _app(fake_bot, db, fake_clock)
    fake_bot.admins = [fake_admin(BOT_ID)]
    await db.set_setting(CHAT_ID, "flood_limit", 2)
    await db.set_setting(CHAT_ID, "flood_window", 10)
    middleware = ContextMiddleware(app)

    async def next_handler(event: Any, data: Any) -> None:
        return None

    for index in range(3):
        await middleware(next_handler, _message("flood", user_id=TARGET_ID, message_id=index), {})
    assert fake_bot.called("restrict_chat_member")
    assert fake_bot.called("delete_message")


async def test_scan_treats_missing_sender_as_anonymous(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """``_scan`` handles a message with no ``from_user`` (id 0, not a bot)."""
    app = await _app(fake_bot, db, fake_clock)
    settings = await app.settings.get(CHAT_ID)
    msg = Message(message_id=1, date=FIXED_DATE, chat=_CHAT, text="hi")
    result = await ContextMiddleware(app)._scan(msg, settings)
    assert result.verdict is Verdict.ALLOW


# --- MessageHandler branches -----------------------------------------------


async def test_message_handler_ignores_bot_sender(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """A message authored by a bot is ignored before any dispatch."""
    app = await _app(fake_bot, db, fake_clock)
    msg = _group_message("/help", user_id=BOT_ID, is_bot=True).as_(app.bot)
    await MessageHandler(app).handle(msg)
    assert fake_bot.called("send_message") == []


async def test_message_handler_ignores_unknown_command(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """An unrecognised command name is dropped silently."""
    app = await _app(fake_bot, db, fake_clock)
    msg = _group_message("/bogus", user_id=ADMIN_ID).as_(app.bot)
    await MessageHandler(app).handle(msg)
    assert fake_bot.called("send_message") == []


async def test_message_handler_non_command_without_members(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """Plain chatter that is not a legacy join does nothing."""
    app = await _app(fake_bot, db, fake_clock)
    msg = _group_message("just chatting", user_id=ADMIN_ID).as_(app.bot)
    await MessageHandler(app).handle(msg)
    assert fake_bot.called("send_message") == []


async def test_message_handler_legacy_join_sends_welcome(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """A legacy ``new_chat_members`` message delivers the welcome."""
    app = await _app(fake_bot, db, fake_clock)
    await db.set_setting(CHAT_ID, "welcome_enabled", 1)
    await db.set_setting(CHAT_ID, "welcome_text", "Hi {first}")
    member = User(id=TARGET_ID, is_bot=False, first_name="Newbie")
    msg = _group_message(None, user_id=ADMIN_ID, new_chat_members=[member]).as_(app.bot)
    await MessageHandler(app).handle(msg, settings=await app.settings.get(CHAT_ID))
    assert any("Hi Newbie" in text for text in fake_bot.texts())


def test_target_label_uses_reply_sender() -> None:
    """A replied-to sender with a username becomes the target label."""
    reply = Message(
        message_id=2,
        date=FIXED_DATE,
        chat=_CHAT,
        from_user=User(id=TARGET_ID, is_bot=False, first_name="Target", username="tgt"),
        text="hello",
    )
    msg = _group_message("/ban", user_id=ADMIN_ID, reply_to=reply)
    assert _target_label(msg) == "tgt"


def test_target_label_falls_back_to_first_name() -> None:
    """Without a username the label is the reply sender's first name."""
    reply = Message(
        message_id=2,
        date=FIXED_DATE,
        chat=_CHAT,
        from_user=User(id=TARGET_ID, is_bot=False, first_name="Target"),
        text="hello",
    )
    msg = _group_message("/ban", user_id=ADMIN_ID, reply_to=reply)
    assert _target_label(msg) == "Target"


def test_target_label_empty_without_reply() -> None:
    """A command that is not a reply has an empty target label."""
    assert _target_label(_group_message("/ban", user_id=ADMIN_ID)) == ""


# --- CallbackHandler branches ----------------------------------------------


async def test_callback_malformed_captcha_is_ignored(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """A captcha-prefixed payload that fails to parse is dropped."""
    app = await _app(fake_bot, db, fake_clock)
    query = CallbackQuery(
        id="1",
        from_user=_user(TARGET_ID),
        chat_instance="ci",
        data="cap:not-a-number:x:y",
    ).as_(app.bot)
    await CallbackHandler(app).handle(query)
    assert fake_bot.called("answer_callback_query") == []


async def test_callback_unrelated_data_is_ignored(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """A callback payload that is neither a captcha nor a settings press is dropped."""
    app = await _app(fake_bot, db, fake_clock)
    query = CallbackQuery(
        id="1",
        from_user=_user(TARGET_ID),
        chat_instance="ci",
        data="something-else",
    ).as_(app.bot)
    await CallbackHandler(app).handle(query)
    assert fake_bot.called("answer_callback_query") == []


async def test_toggle_welcome_without_message(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """Toggling the welcome still works when the panel message is gone."""
    app = await _app(fake_bot, db, fake_clock)
    query = CallbackQuery(
        id="1",
        from_user=_user(ADMIN_ID),
        chat_instance="ci",
        data=f"set:{CHAT_ID}:{ADMIN_ID}:welcome",
    ).as_(app.bot)
    await CallbackHandler(app).handle(query)
    assert (await db.get_settings(CHAT_ID)).welcome_enabled is True
    assert fake_bot.called("edit_message_text") == []


async def test_toggle_welcome_edit_failure_is_swallowed(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """A failing panel edit is logged, not raised."""
    app = await _app(fake_bot, db, fake_clock)
    fake_bot.fail = {"edit_message_text"}
    message = Message(
        message_id=5,
        date=FIXED_DATE,
        chat=_CHAT,
        from_user=_user(ADMIN_ID),
        text="panel",
    )
    query = CallbackQuery(
        id="1",
        from_user=_user(ADMIN_ID),
        chat_instance="ci",
        data=f"set:{CHAT_ID}:{ADMIN_ID}:welcome",
        message=message,
    ).as_(app.bot)
    await CallbackHandler(app).handle(query)
    assert (await db.get_settings(CHAT_ID)).welcome_enabled is True


# --- build_dispatcher ------------------------------------------------------


async def test_build_dispatcher_registers_handlers_and_hooks(
    fake_bot: FakeBot,
    db: Database,
    fake_clock: FakeClock,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """``build_dispatcher`` wires handlers, middlewares, and the lifecycle hooks."""
    app = await _app(fake_bot, db, fake_clock)
    app.dp = Dispatcher()
    builtin_update_mw = len(app.dp.update.outer_middleware)
    started: list[str] = []
    stopped: list[str] = []

    def _start(name: str) -> None:
        started.append(name)

    async def _stop(name: str) -> None:
        stopped.append(name)

    monkeypatch.setattr(app.expiry, "start", lambda: _start("expiry"))
    monkeypatch.setattr(app.captcha_sweeper, "start", lambda: _start("captcha"))
    monkeypatch.setattr(app.expiry, "stop", lambda: _stop("expiry"))
    monkeypatch.setattr(app.captcha_sweeper, "stop", lambda: _stop("captcha"))

    dp = build_dispatcher(app)

    assert dp is app.dp
    assert len(dp.message.handlers) == 1
    assert len(dp.chat_member.handlers) == 1
    assert len(dp.my_chat_member.handlers) == 1
    assert len(dp.callback_query.handlers) == 1
    assert len(dp.message.outer_middleware) == 1
    # A bare App carries no metrics, so only aiogram's own update middlewares remain.
    assert len(dp.update.outer_middleware) == builtin_update_mw

    await dp.emit_startup()
    assert started == ["expiry", "captcha"]
    await dp.emit_shutdown()
    assert stopped == ["expiry", "captcha"]


async def test_build_dispatcher_registers_latency_when_metrics_present(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """With metrics attached, the latency middleware is registered outermost."""
    app = await _app(fake_bot, db, fake_clock)
    app.dp = Dispatcher()
    builtin_update_mw = len(app.dp.update.outer_middleware)
    app.metrics = Metrics()
    dp = build_dispatcher(app)
    assert len(dp.update.outer_middleware) == builtin_update_mw + 1
    assert isinstance(dp.update.outer_middleware[-1], LatencyMiddleware)
