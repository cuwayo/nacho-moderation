"""Tests for the warn command handlers."""

from __future__ import annotations

from nacho_moderator.clock import FakeClock
from nacho_moderator.commands import warns
from nacho_moderator.db import Database
from nacho_moderator.services.settings import SettingsService
from tests.conftest import ADMIN_ID, TARGET_ID, FakeBot
from tests.helpers import context, message


def _ctx(bot: FakeBot, db: Database, clock: FakeClock, *, args: str = "", target_id=TARGET_ID):
    msg = message(11, text="/warn", user_id=ADMIN_ID, username="admin")
    return context(
        bot=bot,
        message=msg,
        args=args,
        target_id=target_id,
        target_label="spammer",
        db=db,
        clock=clock,
        settings=SettingsService(db=db, clock=clock),
    )


async def test_warn_under_limit_reports_count(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock, args="being rude")
    await warns.cmd_warn(ctx)
    assert await db.count_warns(ctx.chat_id, TARGET_ID) == 1
    assert "1/3" in fake_bot.texts()[-1]


async def test_warn_at_limit_applies_default_mute(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    for _ in range(3):
        await warns.cmd_warn(_ctx(fake_bot, db, fake_clock, args="spam"))
    assert fake_bot.called("restrict_chat_member")
    # Warnings are cleared after the action fires.
    assert await db.count_warns(-1001234567890, TARGET_ID) == 0


async def test_warn_at_limit_honours_configured_action(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await db.set_setting(-1001234567890, "warn_action", "ban")
    for _ in range(3):
        await warns.cmd_warn(_ctx(fake_bot, db, fake_clock, args="spam"))
    assert fake_bot.called("ban_chat_member")


async def test_warn_limit_zero_never_auto_acts(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await db.set_setting(-1001234567890, "warn_limit", 0)
    for _ in range(5):
        await warns.cmd_warn(_ctx(fake_bot, db, fake_clock, args="spam"))
    assert fake_bot.called("restrict_chat_member") == []


async def test_unwarn_removes_newest(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await warns.cmd_warn(_ctx(fake_bot, db, fake_clock, args="one"))
    await warns.cmd_warn(_ctx(fake_bot, db, fake_clock, args="two"))
    await warns.cmd_unwarn(_ctx(fake_bot, db, fake_clock))
    rows = await db.list_warns(-1001234567890, TARGET_ID, 10)
    assert [r.reason for r in rows] == ["one"]


async def test_warns_lists_own(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    await warns.cmd_warn(_ctx(fake_bot, db, fake_clock, args="spam"))
    await warns.cmd_warns(_ctx(fake_bot, db, fake_clock, target_id=ADMIN_ID))
    # ADMIN_ID listing their own warnings shows the "You have" header.
    assert any("You have" in t for t in fake_bot.texts())


async def test_resetwarns(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    await warns.cmd_warn(_ctx(fake_bot, db, fake_clock, args="spam"))
    await warns.cmd_resetwarns(_ctx(fake_bot, db, fake_clock))
    assert await db.count_warns(-1001234567890, TARGET_ID) == 0


async def test_setwarnlimit(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    await warns.cmd_setwarnlimit(_ctx(fake_bot, db, fake_clock, args="5"))
    settings = await db.get_settings(-1001234567890)
    assert settings.warn_limit == 5


async def test_setwarnlimit_rejects_garbage(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await warns.cmd_setwarnlimit(_ctx(fake_bot, db, fake_clock, args="lots"))
    assert "Usage" in fake_bot.texts()[-1]


async def test_setwarnaction_valid(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    await warns.cmd_setwarnaction(_ctx(fake_bot, db, fake_clock, args="ban"))
    settings = await db.get_settings(-1001234567890)
    assert settings.warn_action == "ban"


async def test_setwarnaction_rejects_unknown(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await warns.cmd_setwarnaction(_ctx(fake_bot, db, fake_clock, args="explode"))
    assert "Usage" in fake_bot.texts()[-1]
