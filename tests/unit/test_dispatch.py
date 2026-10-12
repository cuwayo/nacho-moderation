"""Tests for the dispatch layer: MessageHandler guards and the automod middleware.

These build real aiogram ``Message``/``ChatMemberUpdated`` objects bound to a
:class:`FakeBot`, so the tests exercise *our* routing, permission gating, and
automod decisions rather than the library.
"""

from __future__ import annotations

from aiogram.types import Chat, Message, MessageEntity, User

from nacho_moderator.clock import FakeClock
from nacho_moderator.constants import Right, Role
from nacho_moderator.db import Database
from nacho_moderator.handlers import App, ContextMiddleware, MessageHandler
from nacho_moderator.services.automod import AutomodService
from nacho_moderator.services.captcha import CaptchaService
from nacho_moderator.services.expiry import CaptchaSweeper, ExpirySweeper
from nacho_moderator.services.perms import AdminCache, Subject, subject_for_user
from nacho_moderator.services.settings import SettingsService
from tests.conftest import ADMIN_ID, BOT_ID, CHAT_ID, TARGET_ID, FakeBot, fake_admin
from tests.helpers import FIXED_DATE

_CHAT = Chat(id=CHAT_ID, type="supergroup", title="Testers")

# The bot holding every right, as getChatAdministrators would report it.
_BOT_SUBJECT = Subject(BOT_ID, Role.ADMIN, Right.RESTRICT | Right.DELETE | Right.PIN)
_BOT_ADMINS = {BOT_ID: _BOT_SUBJECT}

# The bot with the promote right (needed by /promote and /demote).
_BOT_PROMOTE_SUBJECT = Subject(BOT_ID, Role.ADMIN, Right.PROMOTE)
_BOT_PROMOTE_ADMINS = {BOT_ID: _BOT_PROMOTE_SUBJECT}


class _FixedRandom:
    def token_hex(self, nbytes: int) -> str:
        return "deadbeef"[: nbytes * 2]


def _message(
    text: str,
    *,
    user_id: int,
    message_id: int = 1,
    reply_to: Message | None = None,
    entities: list[MessageEntity] | None = None,
    is_bot: bool = False,
) -> Message:
    return Message(
        message_id=message_id,
        date=FIXED_DATE,
        chat=_CHAT,
        from_user=User(id=user_id, is_bot=is_bot, first_name="User", username="u"),
        text=text,
        entities=entities,
        reply_to_message=reply_to,
    )


async def _app(fake_bot: FakeBot, db: Database, clock: FakeClock) -> App:
    settings_service = SettingsService(db=db, clock=clock)
    captcha = CaptchaService(db=db, clock=clock, random=_FixedRandom(), bot=fake_bot)  # type: ignore[arg-type]
    return App(
        config=None,  # type: ignore[arg-type]
        bot=fake_bot,  # type: ignore[arg-type]
        dp=None,  # type: ignore[arg-type]
        db=db,
        settings=settings_service,
        admins=AdminCache(bot=fake_bot, clock=clock),  # type: ignore[arg-type]
        automod=AutomodService(db=db, clock=clock),
        captcha=captcha,
        captcha_sweeper=CaptchaSweeper(captcha),
        expiry=ExpirySweeper(db=db, clock=clock, bot=fake_bot),  # type: ignore[arg-type]
        sudo_ids=set(),
        clock=clock,
        bot_username="mock_bot",
        bot_id=BOT_ID,
    )


async def _dispatch(
    app: App, message: Message, *, caller: Subject, admins: dict[int, Subject]
) -> list[str]:
    """Run MessageHandler with pre-resolved caller/admins (as the middleware would)."""
    handler = MessageHandler(app)
    settings = await app.settings.get(CHAT_ID)
    msg = message.as_(app.bot)
    await handler.handle(msg, caller=caller, admins=admins, settings=settings)
    return app.bot.names()  # type: ignore[attr-defined]


async def test_member_command_denied(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    caller = Subject(TARGET_ID, Role.ANYONE)
    names = await _dispatch(
        app, _message("/ban", user_id=TARGET_ID), caller=caller, admins=_BOT_ADMINS
    )
    assert "ban_chat_member" not in names
    assert any("not allowed" in t for t in fake_bot.texts())


async def test_admin_without_right_denied(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    caller = Subject(ADMIN_ID, Role.ADMIN, Right.DELETE)  # lacks RESTRICT
    await _dispatch(app, _message("/ban 5", user_id=ADMIN_ID), caller=caller, admins=_BOT_ADMINS)
    assert fake_bot.called("ban_chat_member") == []
    assert any("not allowed" in t for t in fake_bot.texts())


async def test_bot_lacking_right_reports_bot(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    caller = Subject(ADMIN_ID, Role.ADMIN, Right.RESTRICT)
    admins = {BOT_ID: Subject(BOT_ID, Role.ADMIN, Right.DELETE)}  # bot lacks RESTRICT
    await _dispatch(app, _message("/ban 5", user_id=ADMIN_ID), caller=caller, admins=admins)
    assert any("I lack the rights" in t for t in fake_bot.texts())


async def test_group_only_command_in_private(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    private_chat = Chat(id=1, type="private")
    msg = Message(
        message_id=1,
        date=FIXED_DATE,
        chat=private_chat,
        from_user=User(id=ADMIN_ID, is_bot=False, first_name="A"),
        text="/ban 5",
    ).as_(app.bot)
    caller = Subject(ADMIN_ID, Role.OWNER)
    await MessageHandler(app).handle(
        msg, caller=caller, admins={}, settings=await app.settings.get(1)
    )
    assert any("only available in groups" in t for t in fake_bot.texts())


async def test_cannot_act_on_admin_target(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    caller = Subject(ADMIN_ID, Role.ADMIN, Right.RESTRICT)
    admins = {**_BOT_ADMINS, ADMIN_ID: Subject(ADMIN_ID, Role.ADMIN, Right.RESTRICT)}
    await _dispatch(
        app, _message(f"/ban {ADMIN_ID}", user_id=ADMIN_ID), caller=caller, admins=admins
    )
    assert fake_bot.called("ban_chat_member") == []
    assert any("cannot act" in t for t in fake_bot.texts())


async def test_unresolvable_username_replies(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    caller = Subject(ADMIN_ID, Role.ADMIN, Right.RESTRICT)
    await _dispatch(
        app, _message("/ban @ghost", user_id=ADMIN_ID), caller=caller, admins=_BOT_ADMINS
    )
    assert fake_bot.called("ban_chat_member") == []
    assert any("do not know that username" in t for t in fake_bot.texts())


async def test_message_for_other_bot_ignored(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    caller = Subject(ADMIN_ID, Role.OWNER)
    await _dispatch(
        app, _message("/help@otherbot", user_id=ADMIN_ID), caller=caller, admins=_BOT_ADMINS
    )
    assert fake_bot.called("send_message") == []


# --- automod middleware ----------------------------------------------------


async def test_middleware_exempts_admin(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    fake_bot.admins = [fake_admin(ADMIN_ID), fake_admin(BOT_ID)]
    await db.set_setting(CHAT_ID, "locks", 1 << 0)  # lock links
    middleware = ContextMiddleware(app)
    seen: list[bool] = []

    async def next_handler(event, data):
        seen.append(True)

    msg = _message(
        "https://example.com",
        user_id=ADMIN_ID,
        entities=[MessageEntity(type="url", offset=0, length=19)],
    )
    await middleware(next_handler, msg, {})
    assert seen == [True]  # admin reached the handler; nothing deleted
    assert fake_bot.called("delete_message") == []


async def test_middleware_deletes_locked_link(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    fake_bot.admins = [fake_admin(BOT_ID)]  # TARGET_ID is not an admin
    await db.set_setting(CHAT_ID, "locks", 1 << 0)  # lock links
    middleware = ContextMiddleware(app)
    reached: list[bool] = []

    async def next_handler(event, data):
        reached.append(True)

    msg = _message(
        "https://example.com",
        user_id=TARGET_ID,
        entities=[MessageEntity(type="url", offset=0, length=19)],
    )
    await middleware(next_handler, msg, {})
    assert reached == []  # propagation stopped
    assert fake_bot.called("delete_message")


async def test_middleware_lets_clean_message_through(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    fake_bot.admins = [fake_admin(BOT_ID)]
    middleware = ContextMiddleware(app)
    reached: list[bool] = []

    async def next_handler(event, data):
        reached.append(True)

    await middleware(next_handler, _message("hello", user_id=TARGET_ID), {})
    assert reached == [True]
    assert fake_bot.called("delete_message") == []


async def test_middleware_skips_command_messages(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    fake_bot.admins = [fake_admin(BOT_ID)]
    middleware = ContextMiddleware(app)
    reached: list[bool] = []

    async def next_handler(event, data):
        reached.append(True)

    await middleware(next_handler, _message("/help", user_id=TARGET_ID), {})
    assert reached == [True]


async def test_middleware_mutes_on_antiflood(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    fake_bot.admins = [fake_admin(BOT_ID)]
    await db.set_setting(CHAT_ID, "flood_limit", 2)
    await db.set_setting(CHAT_ID, "flood_window", 10)
    middleware = ContextMiddleware(app)

    async def next_handler(event, data):
        return None

    for i in range(4):
        await middleware(next_handler, _message("flood", user_id=TARGET_ID, message_id=i), {})
    assert fake_bot.called("restrict_chat_member")
    assert fake_bot.called("delete_message")


async def test_subject_for_user_from_admins() -> None:
    admins = {ADMIN_ID: Subject(ADMIN_ID, Role.ADMIN, Right.RESTRICT)}
    subject = subject_for_user(ADMIN_ID, admins, set())
    assert subject.role is Role.ADMIN


# --- /promote and /demote permission matrix --------------------------------


async def test_promote_admin_with_right_allowed(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    caller = Subject(ADMIN_ID, Role.ADMIN, Right.PROMOTE)
    await _dispatch(
        app,
        _message(f"/promote {TARGET_ID}", user_id=ADMIN_ID),
        caller=caller,
        admins=_BOT_PROMOTE_ADMINS,
    )
    assert fake_bot.called("promote_chat_member")


async def test_promote_admin_without_right_denied(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    caller = Subject(ADMIN_ID, Role.ADMIN, Right.DELETE)  # lacks PROMOTE
    await _dispatch(
        app,
        _message(f"/promote {TARGET_ID}", user_id=ADMIN_ID),
        caller=caller,
        admins=_BOT_PROMOTE_ADMINS,
    )
    assert fake_bot.called("promote_chat_member") == []
    assert any("not allowed" in t for t in fake_bot.texts())


async def test_promote_member_denied(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    caller = Subject(TARGET_ID, Role.ANYONE)
    await _dispatch(
        app,
        _message(f"/promote {ADMIN_ID}", user_id=TARGET_ID),
        caller=caller,
        admins=_BOT_PROMOTE_ADMINS,
    )
    assert fake_bot.called("promote_chat_member") == []


async def test_promote_owner_allowed(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    caller = Subject(ADMIN_ID, Role.OWNER)
    await _dispatch(
        app,
        _message(f"/promote {TARGET_ID}", user_id=ADMIN_ID),
        caller=caller,
        admins=_BOT_PROMOTE_ADMINS,
    )
    assert fake_bot.called("promote_chat_member")


async def test_promote_sudo_allowed(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    app = await _app(fake_bot, db, fake_clock)
    caller = Subject(ADMIN_ID, Role.SUDO, Right.NONE, is_sudo=True)
    await _dispatch(
        app,
        _message(f"/promote {TARGET_ID}", user_id=ADMIN_ID),
        caller=caller,
        admins=_BOT_PROMOTE_ADMINS,
    )
    assert fake_bot.called("promote_chat_member")


async def test_promote_bot_lacking_right_reports(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    caller = Subject(ADMIN_ID, Role.ADMIN, Right.PROMOTE)
    admins = {BOT_ID: Subject(BOT_ID, Role.ADMIN, Right.DELETE)}  # bot lacks PROMOTE
    await _dispatch(
        app, _message(f"/promote {TARGET_ID}", user_id=ADMIN_ID), caller=caller, admins=admins
    )
    assert fake_bot.called("promote_chat_member") == []
    assert any("I lack the rights" in t for t in fake_bot.texts())


async def test_promote_owner_target_blocked_by_generic_guard(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """Promoting the owner is blocked by the standard protected-target guard."""
    app = await _app(fake_bot, db, fake_clock)
    caller = Subject(ADMIN_ID, Role.ADMIN, Right.PROMOTE)
    admins = {**_BOT_PROMOTE_ADMINS, ADMIN_ID: Subject(ADMIN_ID, Role.OWNER)}
    await _dispatch(
        app, _message(f"/promote {ADMIN_ID}", user_id=ADMIN_ID), caller=caller, admins=admins
    )
    assert fake_bot.called("promote_chat_member") == []
    assert any("cannot act" in t for t in fake_bot.texts())


async def test_demote_admin_target_allowed(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """ADMIN_TARGET lets /demote act on an administrator the generic guard blocks."""
    app = await _app(fake_bot, db, fake_clock)
    caller = Subject(ADMIN_ID, Role.ADMIN, Right.PROMOTE)
    admins = {
        **_BOT_PROMOTE_ADMINS,
        TARGET_ID: Subject(TARGET_ID, Role.ADMIN, Right.RESTRICT),
    }
    await _dispatch(
        app, _message(f"/demote {TARGET_ID}", user_id=ADMIN_ID), caller=caller, admins=admins
    )
    assert fake_bot.called("promote_chat_member")


async def test_demote_owner_target_refused(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    caller = Subject(ADMIN_ID, Role.ADMIN, Right.PROMOTE)
    admins = {**_BOT_PROMOTE_ADMINS, TARGET_ID: Subject(TARGET_ID, Role.OWNER)}
    await _dispatch(
        app, _message(f"/demote {TARGET_ID}", user_id=ADMIN_ID), caller=caller, admins=admins
    )
    assert fake_bot.called("promote_chat_member") == []
    assert any("owner" in t for t in fake_bot.texts())


async def test_promote_requires_target(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    caller = Subject(ADMIN_ID, Role.ADMIN, Right.PROMOTE)
    await _dispatch(
        app, _message("/promote", user_id=ADMIN_ID), caller=caller, admins=_BOT_PROMOTE_ADMINS
    )
    assert fake_bot.called("promote_chat_member") == []
    assert any("Reply to a user" in t for t in fake_bot.texts())


async def test_start_works_in_private_chat(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """The only command that works in a private chat besides /help."""
    app = await _app(fake_bot, db, fake_clock)
    private_chat = Chat(id=1, type="private")
    msg = Message(
        message_id=1,
        date=FIXED_DATE,
        chat=private_chat,
        from_user=User(id=ADMIN_ID, is_bot=False, first_name="A"),
        text="/start",
    ).as_(app.bot)
    caller = Subject(ADMIN_ID, Role.ANYONE)
    await MessageHandler(app).handle(
        msg, caller=caller, admins={}, settings=await app.settings.get(1)
    )
    assert any("Available commands" in t for t in fake_bot.texts())


async def test_promote_group_only_in_private(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    app = await _app(fake_bot, db, fake_clock)
    private_chat = Chat(id=1, type="private")
    msg = Message(
        message_id=1,
        date=FIXED_DATE,
        chat=private_chat,
        from_user=User(id=ADMIN_ID, is_bot=False, first_name="A"),
        text=f"/promote {TARGET_ID}",
    ).as_(app.bot)
    caller = Subject(ADMIN_ID, Role.ADMIN, Right.PROMOTE)
    await MessageHandler(app).handle(
        msg, caller=caller, admins=_BOT_PROMOTE_ADMINS, settings=await app.settings.get(1)
    )
    assert fake_bot.called("promote_chat_member") == []
    assert any("only available in groups" in t for t in fake_bot.texts())
