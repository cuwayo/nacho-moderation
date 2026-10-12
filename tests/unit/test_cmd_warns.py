"""Tests for the warn command handlers."""

from __future__ import annotations

from nacho_moderator.clock import FakeClock
from nacho_moderator.commands import warns
from nacho_moderator.constants import Role
from nacho_moderator.db import Database
from nacho_moderator.services.perms import Subject
from nacho_moderator.services.settings import SettingsService
from tests.conftest import ADMIN_ID, CHAT_ID, TARGET_ID, FakeBot
from tests.helpers import context, message


def _ctx(
    bot: FakeBot,
    db: Database,
    clock: FakeClock,
    *,
    args: str = "",
    target_id: int | None = TARGET_ID,
    caller: Subject | None = None,
    target_label: str = "spammer",
):
    msg = message(11, text="/warn", user_id=ADMIN_ID, username="admin")
    return context(
        bot=bot,
        message=msg,
        args=args,
        target_id=target_id,
        target_label=target_label,
        caller=caller,
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


async def test_warn_without_target_replies_usage(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await warns.cmd_warn(_ctx(fake_bot, db, fake_clock, target_id=None))
    assert "Reply to a user" in fake_bot.texts()[-1]
    assert await db.count_warns(CHAT_ID, TARGET_ID) == 0


async def test_warn_limit_reached_with_ban_action(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await db.set_setting(CHAT_ID, "warn_action", "ban")
    for _ in range(3):
        await warns.cmd_warn(_ctx(fake_bot, db, fake_clock, args="spam"))
    calls = fake_bot.called("ban_chat_member")
    assert len(calls) == 1
    assert calls[0].kwargs["user_id"] == TARGET_ID
    assert calls[0].kwargs["revoke_messages"] is True
    # A ban does not unban.
    assert fake_bot.called("unban_chat_member") == []
    assert any("limit" in t for t in fake_bot.texts())


async def test_warn_limit_reached_with_kick_action(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await db.set_setting(CHAT_ID, "warn_action", "kick")
    for _ in range(3):
        await warns.cmd_warn(_ctx(fake_bot, db, fake_clock, args="spam"))
    assert fake_bot.called("ban_chat_member")
    unban = fake_bot.called("unban_chat_member")
    assert len(unban) == 1
    assert unban[0].kwargs["only_if_banned"] is True


async def test_warn_uses_generic_label_when_unknown(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await warns.cmd_warn(_ctx(fake_bot, db, fake_clock, args="spam", target_label=""))
    assert "the user" in fake_bot.texts()[-1]


async def test_unwarn_when_nothing_to_remove(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await warns.cmd_unwarn(_ctx(fake_bot, db, fake_clock))
    assert fake_bot.texts()[-1] == "That failed."


async def test_unwarn_without_target_replies_usage(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await warns.cmd_unwarn(_ctx(fake_bot, db, fake_clock, target_id=None))
    assert "Reply to a user" in fake_bot.texts()[-1]


async def test_warns_admin_lists_another_user(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await warns.cmd_warn(_ctx(fake_bot, db, fake_clock, args="spam"))
    await warns.cmd_warns(_ctx(fake_bot, db, fake_clock))
    text = fake_bot.texts()[-1]
    assert "spammer has 1 warning(s):" in text
    assert "1. spam" in text


async def test_warns_formats_missing_reason(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await warns.cmd_warn(_ctx(fake_bot, db, fake_clock, args=""))
    await warns.cmd_warns(_ctx(fake_bot, db, fake_clock))
    assert "(no reason)" in fake_bot.texts()[-1]


async def test_warns_plain_member_cannot_list_another_user(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    member = Subject(ADMIN_ID, Role.ANYONE, username="member")
    await warns.cmd_warns(_ctx(fake_bot, db, fake_clock, caller=member))
    assert "Only administrators" in fake_bot.texts()[-1]


async def test_warns_plain_member_lists_own(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    member = Subject(ADMIN_ID, Role.ANYONE, username="member")
    await warns.cmd_warn(_ctx(fake_bot, db, fake_clock, args="spam", target_id=ADMIN_ID))
    await warns.cmd_warns(_ctx(fake_bot, db, fake_clock, caller=member, target_id=None))
    assert "You have 1 warning(s):" in fake_bot.texts()[-1]


async def test_resetwarns_without_target_replies_usage(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await warns.cmd_resetwarns(_ctx(fake_bot, db, fake_clock, target_id=None))
    assert "Reply to a user" in fake_bot.texts()[-1]


async def test_setwarnlimit_rejects_over_1000(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await warns.cmd_setwarnlimit(_ctx(fake_bot, db, fake_clock, args="1001"))
    assert "Usage" in fake_bot.texts()[-1]
    assert (await db.get_settings(CHAT_ID)).warn_limit == 3
