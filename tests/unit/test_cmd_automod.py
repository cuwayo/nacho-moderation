"""Tests for the automod configuration commands (lock/unlock/blacklist/antiflood/captcha)."""

from __future__ import annotations

from nacho_moderator.clock import FakeClock
from nacho_moderator.commands import automod_cmds
from nacho_moderator.constants import LockType
from nacho_moderator.db import Database
from nacho_moderator.services.automod import AutomodService
from nacho_moderator.services.settings import SettingsService
from tests.conftest import ADMIN_ID, CHAT_ID, FakeBot
from tests.helpers import context, message


def _ctx(bot: FakeBot, db: Database, clock: FakeClock, *, args: str = "", automod=None):
    msg = message(11, text="/cmd", user_id=ADMIN_ID, username="admin")
    return context(
        bot=bot,
        message=msg,
        args=args,
        db=db,
        clock=clock,
        automod=automod,
        settings=SettingsService(db=db, clock=clock),
    )


async def test_lock_sets_bit(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    await automod_cmds.cmd_lock(_ctx(fake_bot, db, fake_clock, args="links media"))
    settings = await db.get_settings(CHAT_ID)
    assert settings.locks & (1 << int(LockType.LINKS))
    assert settings.locks & (1 << int(LockType.MEDIA))
    assert fake_bot.texts() == ["Locked."]


async def test_unlock_clears_bit(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    await automod_cmds.cmd_lock(_ctx(fake_bot, db, fake_clock, args="links"))
    await automod_cmds.cmd_unlock(_ctx(fake_bot, db, fake_clock, args="links"))
    settings = await db.get_settings(CHAT_ID)
    assert not (settings.locks & (1 << int(LockType.LINKS)))


async def test_lock_unknown_type(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    await automod_cmds.cmd_lock(_ctx(fake_bot, db, fake_clock, args="telepathy"))
    assert "Unknown lock type" in fake_bot.texts()[-1]


async def test_lock_without_args_shows_usage(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await automod_cmds.cmd_lock(_ctx(fake_bot, db, fake_clock, args=""))
    assert "Usage" in fake_bot.texts()[-1]


async def test_blacklist_add_and_list(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    automod = AutomodService(db=db, clock=fake_clock)
    await automod_cmds.cmd_blacklist(
        _ctx(fake_bot, db, fake_clock, args="add badword", automod=automod)
    )
    assert "Added" in fake_bot.texts()[-1]
    await automod_cmds.cmd_blacklist(_ctx(fake_bot, db, fake_clock, args="list"))
    assert "badword" in fake_bot.texts()[-1]


async def test_blacklist_add_regex(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    automod = AutomodService(db=db, clock=fake_clock)
    await automod_cmds.cmd_blacklist(
        _ctx(fake_bot, db, fake_clock, args="add /sp\\w+/", automod=automod)
    )
    entries = await db.blacklist_list(CHAT_ID, 16)
    assert entries[0].is_regex is True
    assert entries[0].pattern == r"sp\w+"


async def test_blacklist_invalid_regex_rejected(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    automod = AutomodService(db=db, clock=fake_clock)
    await automod_cmds.cmd_blacklist(
        _ctx(fake_bot, db, fake_clock, args="add /(/", automod=automod)
    )
    assert "not a valid" in fake_bot.texts()[-1]
    assert await db.blacklist_list(CHAT_ID, 16) == []


async def test_blacklist_del(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    automod = AutomodService(db=db, clock=fake_clock)
    await db.blacklist_add(CHAT_ID, "badword", is_regex=False)
    await automod_cmds.cmd_blacklist(
        _ctx(fake_bot, db, fake_clock, args="del badword", automod=automod)
    )
    assert await db.blacklist_list(CHAT_ID, 16) == []


async def test_antiflood_set_and_off(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await automod_cmds.cmd_antiflood(_ctx(fake_bot, db, fake_clock, args="5 10"))
    settings = await db.get_settings(CHAT_ID)
    assert settings.flood_limit == 5
    assert settings.flood_window == 10
    await automod_cmds.cmd_antiflood(_ctx(fake_bot, db, fake_clock, args="off"))
    assert (await db.get_settings(CHAT_ID)).flood_limit == 0


async def test_antiflood_rejects_bad_args(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await automod_cmds.cmd_antiflood(_ctx(fake_bot, db, fake_clock, args="lots"))
    assert "Usage" in fake_bot.texts()[-1]


async def test_captcha_toggle(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    await automod_cmds.cmd_captcha(_ctx(fake_bot, db, fake_clock, args="on"))
    assert (await db.get_settings(CHAT_ID)).captcha_enabled is True
    await automod_cmds.cmd_captcha(_ctx(fake_bot, db, fake_clock, args="off"))
    assert (await db.get_settings(CHAT_ID)).captcha_enabled is False


async def test_captcha_rejects_other_args(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await automod_cmds.cmd_captcha(_ctx(fake_bot, db, fake_clock, args="maybe"))
    assert "Usage" in fake_bot.texts()[-1]
