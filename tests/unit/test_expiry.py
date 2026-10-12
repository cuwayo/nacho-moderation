"""Unit tests for the expiry sweeper (temp ban/mute lifting)."""

from __future__ import annotations

from unittest.mock import AsyncMock

from nacho_moderator.clock import FakeClock
from nacho_moderator.db import Database
from nacho_moderator.services.expiry import ExpirySweeper


async def test_expired_ban_is_unbanned(db: Database, fake_clock: FakeClock) -> None:
    bot = AsyncMock()
    sweeper = ExpirySweeper(db=db, clock=fake_clock, bot=bot)
    now = int(fake_clock.now())
    await db.record_action(-100, 5, 2, "ban", "spam", now, now + 60)
    fake_clock.advance(61)
    processed = await sweeper.sweep_once()
    assert processed == 1
    bot.unban_chat_member.assert_awaited_once()
    assert await db.due_actions(int(fake_clock.now()), 32) == []


async def test_expired_mute_restores_permissions(db: Database, fake_clock: FakeClock) -> None:
    bot = AsyncMock()
    sweeper = ExpirySweeper(db=db, clock=fake_clock, bot=bot)
    now = int(fake_clock.now())
    await db.record_action(-100, 5, 2, "mute", "flood", now, now + 60)
    fake_clock.advance(61)
    await sweeper.sweep_once()
    bot.restrict_chat_member.assert_awaited_once()


async def test_permanent_action_never_swept(db: Database, fake_clock: FakeClock) -> None:
    bot = AsyncMock()
    sweeper = ExpirySweeper(db=db, clock=fake_clock, bot=bot)
    now = int(fake_clock.now())
    await db.record_action(-100, 5, 2, "ban", "permanent", now, None)
    fake_clock.advance(10_000)
    assert await sweeper.sweep_once() == 0
    bot.unban_chat_member.assert_not_awaited()


async def test_lift_failure_is_logged_not_raised(db: Database, fake_clock: FakeClock) -> None:
    bot = AsyncMock()
    bot.unban_chat_member.side_effect = RuntimeError("boom")
    sweeper = ExpirySweeper(db=db, clock=fake_clock, bot=bot)
    now = int(fake_clock.now())
    await db.record_action(-100, 5, 2, "ban", "spam", now, now + 1)
    fake_clock.advance(2)
    # The row is still marked lifted even though the API call failed.
    assert await sweeper.sweep_once() == 1
    assert await db.due_actions(int(fake_clock.now()), 32) == []
