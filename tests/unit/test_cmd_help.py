"""Tests for the /help command handler."""

from __future__ import annotations

from nacho_moderator.clock import FakeClock
from nacho_moderator.commands import help_cmd
from nacho_moderator.constants import Role
from nacho_moderator.db import Database
from nacho_moderator.services.perms import Subject
from nacho_moderator.services.settings import SettingsService
from tests.conftest import ADMIN_ID, FakeBot
from tests.helpers import context, message


def _ctx(bot: FakeBot, db: Database, clock: FakeClock, role: Role) -> object:
    msg = message(11, text="/help", user_id=ADMIN_ID, username="admin")
    return context(
        bot=bot,
        message=msg,
        db=db,
        clock=clock,
        caller=Subject(ADMIN_ID, role),
        settings=SettingsService(db=db, clock=clock),
    )


async def test_help_member_sees_only_public_commands(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await help_cmd.cmd_help(_ctx(fake_bot, db, fake_clock, Role.ANYONE))  # type: ignore[arg-type]
    text = fake_bot.texts()[-1]
    assert "/help" in text
    assert "/ban" not in text


async def test_help_admin_sees_admin_commands(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    await help_cmd.cmd_help(_ctx(fake_bot, db, fake_clock, Role.ADMIN))  # type: ignore[arg-type]
    text = fake_bot.texts()[-1]
    assert "/ban" in text
    assert "Available commands:" in text
