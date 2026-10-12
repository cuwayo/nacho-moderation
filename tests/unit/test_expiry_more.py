"""Extra unit tests for the expiry sweeper and the captcha sweeper tick."""

from __future__ import annotations

from unittest.mock import AsyncMock

from nacho_moderator.clock import FakeClock
from nacho_moderator.db import Database
from nacho_moderator.services.captcha import CaptchaService
from nacho_moderator.services.expiry import CaptchaSweeper, ExpirySweeper
from tests.conftest import CHAT_ID, TARGET_ID, FakeBot


class _FixedRandom:
    def token_hex(self, nbytes: int) -> str:
        return "deadbeef"[: nbytes * 2]


def test_clock_property_exposes_injected_clock(db: Database, fake_clock: FakeClock) -> None:
    sweeper = ExpirySweeper(db=db, clock=fake_clock, bot=AsyncMock())
    assert sweeper.clock is fake_clock


async def test_tick_lifts_due_actions(db: Database, fake_clock: FakeClock) -> None:
    bot = AsyncMock()
    sweeper = ExpirySweeper(db=db, clock=fake_clock, bot=bot)
    now = int(fake_clock.now())
    await db.record_action(-100, 5, 2, "ban", "spam", now, now + 1)
    fake_clock.advance(2)
    await sweeper._tick()
    bot.unban_chat_member.assert_awaited_once()


async def test_unknown_action_is_ignored(db: Database, fake_clock: FakeClock) -> None:
    """An action that is neither ban nor mute performs no API call."""
    bot = AsyncMock()
    sweeper = ExpirySweeper(db=db, clock=fake_clock, bot=bot)
    await sweeper._lift(-100, 5, "warn")
    bot.unban_chat_member.assert_not_awaited()
    bot.restrict_chat_member.assert_not_awaited()


async def test_captcha_sweeper_tick_kicks_expired(
    db: Database, fake_clock: FakeClock, fake_bot: FakeBot
) -> None:
    captcha = CaptchaService(db=db, clock=fake_clock, random=_FixedRandom(), bot=fake_bot)  # type: ignore[arg-type]
    sweeper = CaptchaSweeper(captcha)
    await captcha.begin(CHAT_ID, TARGET_ID, timeout_seconds=120)
    fake_clock.advance(121)
    await sweeper._tick()
    assert fake_bot.called("ban_chat_member")
    assert await db.captcha_get(CHAT_ID, TARGET_ID) is None
