"""Tests for the captcha service: issue, verify, and expire."""

from __future__ import annotations

from nacho_moderator.clock import FakeClock
from nacho_moderator.db import Database
from nacho_moderator.services.captcha import CaptchaCallback, CaptchaService
from tests.conftest import CHAT_ID, TARGET_ID, FakeBot


class _FixedRandom:
    def token_hex(self, nbytes: int) -> str:
        return "deadbeef"[: nbytes * 2]


def _service(bot: FakeBot, db: Database, clock: FakeClock) -> CaptchaService:
    return CaptchaService(db=db, clock=clock, random=_FixedRandom(), bot=bot)  # type: ignore[arg-type]


async def test_begin_restricts_and_prompts(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    service = _service(fake_bot, db, fake_clock)
    await service.begin(CHAT_ID, TARGET_ID, timeout_seconds=120)
    restrict = fake_bot.called("restrict_chat_member")[0]
    assert restrict.kwargs["permissions"].can_send_messages is False
    assert fake_bot.called("send_message")
    row = await db.captcha_get(CHAT_ID, TARGET_ID)
    assert row is not None
    assert row.nonce == "deadbeef"
    assert row.deadline == int(fake_clock.now()) + 120


async def test_correct_user_verifies(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    service = _service(fake_bot, db, fake_clock)
    await service.begin(CHAT_ID, TARGET_ID, timeout_seconds=120)
    answer = await service.verify(CaptchaCallback(CHAT_ID, TARGET_ID, "deadbeef"), TARGET_ID)
    assert answer == "Verified. Welcome!"
    assert await db.captcha_get(CHAT_ID, TARGET_ID) is None
    # The second restrict is the lift.
    lifts = fake_bot.called("restrict_chat_member")
    assert lifts[-1].kwargs["permissions"].can_send_messages is True


async def test_wrong_user_rejected(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    service = _service(fake_bot, db, fake_clock)
    await service.begin(CHAT_ID, TARGET_ID, timeout_seconds=120)
    answer = await service.verify(CaptchaCallback(CHAT_ID, TARGET_ID, "deadbeef"), 999)
    assert answer == "This challenge is not yours."
    assert await db.captcha_get(CHAT_ID, TARGET_ID) is not None


async def test_wrong_nonce_rejected(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    service = _service(fake_bot, db, fake_clock)
    await service.begin(CHAT_ID, TARGET_ID, timeout_seconds=120)
    answer = await service.verify(CaptchaCallback(CHAT_ID, TARGET_ID, "ffffffff"), TARGET_ID)
    assert answer == "This challenge has expired."


async def test_double_click_is_expired(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    service = _service(fake_bot, db, fake_clock)
    await service.begin(CHAT_ID, TARGET_ID, timeout_seconds=120)
    await service.verify(CaptchaCallback(CHAT_ID, TARGET_ID, "deadbeef"), TARGET_ID)
    answer = await service.verify(CaptchaCallback(CHAT_ID, TARGET_ID, "deadbeef"), TARGET_ID)
    assert answer == "This challenge has expired."


async def test_sweep_kicks_expired(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    service = _service(fake_bot, db, fake_clock)
    await service.begin(CHAT_ID, TARGET_ID, timeout_seconds=120)
    assert await service.sweep_expired() == 0
    fake_clock.advance(121)
    assert await service.sweep_expired() == 1
    assert fake_bot.called("ban_chat_member")
    assert fake_bot.called("unban_chat_member")
    assert await db.captcha_get(CHAT_ID, TARGET_ID) is None


async def test_verify_without_row_is_expired(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    service = _service(fake_bot, db, fake_clock)
    answer = await service.verify(CaptchaCallback(CHAT_ID, TARGET_ID, "deadbeef"), TARGET_ID)
    assert answer == "This challenge has expired."
