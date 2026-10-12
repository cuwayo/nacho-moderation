"""Tests for the settings, welcome, rules, log, and reload command handlers."""

from __future__ import annotations

from nacho_moderator.clock import FakeClock
from nacho_moderator.commands import settings_cmds
from nacho_moderator.constants import Role
from nacho_moderator.db import Database
from nacho_moderator.services.perms import Subject
from nacho_moderator.services.settings import SettingsService
from tests.conftest import ADMIN_ID, CHAT_ID, FakeBot
from tests.helpers import context, message

OWNER = Subject(ADMIN_ID, Role.OWNER, username="admin")


def _ctx(bot: FakeBot, db: Database, clock: FakeClock, *, args: str = "", caller=None):
    msg = message(11, text="/cmd", user_id=ADMIN_ID, username="admin")
    return context(
        bot=bot,
        message=msg,
        args=args,
        db=db,
        clock=clock,
        caller=caller,
        settings=SettingsService(db=db, clock=clock),
    )


async def test_setwelcome_stores_template(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await settings_cmds.cmd_setwelcome(_ctx(fake_bot, db, fake_clock, args="Hi {first}"))
    settings = await db.get_settings(CHAT_ID)
    assert settings.welcome_text == "Hi {first}"


async def test_setwelcome_requires_text(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await settings_cmds.cmd_setwelcome(_ctx(fake_bot, db, fake_clock, args=""))
    assert "Usage" in fake_bot.texts()[-1]


async def test_welcome_toggle(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    await settings_cmds.cmd_welcome(_ctx(fake_bot, db, fake_clock, args="on"))
    assert (await db.get_settings(CHAT_ID)).welcome_enabled is True
    await settings_cmds.cmd_welcome(_ctx(fake_bot, db, fake_clock, args="off"))
    assert (await db.get_settings(CHAT_ID)).welcome_enabled is False


async def test_welcome_rejects_other_args(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await settings_cmds.cmd_welcome(_ctx(fake_bot, db, fake_clock, args="maybe"))
    assert "Usage" in fake_bot.texts()[-1]


async def test_setrules_and_rules(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    await settings_cmds.cmd_setrules(_ctx(fake_bot, db, fake_clock, args="Be nice"))
    await settings_cmds.cmd_rules(_ctx(fake_bot, db, fake_clock))
    assert "Be nice" in fake_bot.texts()[-1]


async def test_rules_when_unset(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    await settings_cmds.cmd_rules(_ctx(fake_bot, db, fake_clock))
    assert "No rules" in fake_bot.texts()[-1]


async def test_setlog_and_off(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    await settings_cmds.cmd_setlog(_ctx(fake_bot, db, fake_clock, args="-100999"))
    assert (await db.get_settings(CHAT_ID)).log_chat_id == -100999
    await settings_cmds.cmd_setlog(_ctx(fake_bot, db, fake_clock, args="off"))
    assert (await db.get_settings(CHAT_ID)).log_chat_id == 0


async def test_setlog_rejects_garbage(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await settings_cmds.cmd_setlog(_ctx(fake_bot, db, fake_clock, args="notanid"))
    assert "Usage" in fake_bot.texts()[-1]


async def test_settings_panel_renders_welcome_state(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await settings_cmds.cmd_settings(_ctx(fake_bot, db, fake_clock))
    send = fake_bot.called("send_message")[-1]
    button = send.kwargs["reply_markup"].inline_keyboard[0][0]
    assert button.text == "Welcome: OFF"
    assert button.callback_data == f"set:{CHAT_ID}:{ADMIN_ID}:welcome"


async def test_reload_invalidates_admin_cache(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    invalidated: list[int] = []
    ctx = _ctx(fake_bot, db, fake_clock)
    ctx.invalidate_admins = invalidated.append
    await settings_cmds.cmd_reload(ctx)
    assert invalidated == [CHAT_ID]
    assert "refreshed" in fake_bot.texts()[-1]


async def test_record_action_posts_to_log_channel(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock)
    ctx.log_chat_id = -100777
    await ctx.record_action("ban", 555, "spam", ctx.now(), None, "spammer")
    logs = [c for c in fake_bot.called("send_message") if c.kwargs["chat_id"] == -100777]
    assert logs
    assert "action=ban" in logs[0].kwargs["text"]
    assert "spammer" in logs[0].kwargs["text"]


async def test_render_panel_text_returns_title(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock)
    assert await settings_cmds.render_panel_text(ctx) == settings_cmds.PANEL_TITLE


async def test_setrules_requires_text(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await settings_cmds.cmd_setrules(_ctx(fake_bot, db, fake_clock, args=""))
    assert "Usage" in fake_bot.texts()[-1]
    assert (await db.get_settings(CHAT_ID)).rules_text == ""


async def test_reload_without_invalidator_still_replies(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    # invalidate_admins defaults to None, so the callable() guard is exercised.
    ctx = _ctx(fake_bot, db, fake_clock)
    await settings_cmds.cmd_reload(ctx)
    assert "refreshed" in fake_bot.texts()[-1]
