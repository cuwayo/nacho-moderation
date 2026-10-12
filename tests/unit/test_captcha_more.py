"""Extra unit tests for the captcha service.

Fills the gaps in ``test_captcha_service.py``: the malformed-callback parse
branches and the best-effort prompt deletion that may fail on both the verify
and the sweep paths.
"""

from __future__ import annotations

from nacho_moderator.clock import FakeClock
from nacho_moderator.db import Database
from nacho_moderator.services.captcha import (
    CaptchaCallback,
    CaptchaService,
    parse_captcha_callback,
)
from tests.conftest import CHAT_ID, TARGET_ID, FakeBot


class _FixedRandom:
    def token_hex(self, nbytes: int) -> str:
        return "deadbeef"[: nbytes * 2]


def _service(bot: FakeBot, db: Database, clock: FakeClock) -> CaptchaService:
    return CaptchaService(db=db, clock=clock, random=_FixedRandom(), bot=bot)  # type: ignore[arg-type]


def test_parse_rejects_non_captcha_prefix() -> None:
    assert parse_captcha_callback("set:-100:1:welcome") is None


def test_parse_rejects_wrong_part_count() -> None:
    assert parse_captcha_callback("cap:-100:1") is None


def test_parse_rejects_empty_nonce() -> None:
    assert parse_captcha_callback("cap:-100:1:") is None


def test_parse_rejects_non_numeric_ids() -> None:
    assert parse_captcha_callback("cap:notanint:1:deadbeef") is None


def test_parse_round_trips_a_well_formed_payload() -> None:
    parsed = parse_captcha_callback("cap:-100:5:abc")
    assert parsed == CaptchaCallback(chat_id=-100, user_id=5, nonce="abc")


async def test_verify_survives_missing_prompt_message(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """A prompt already deleted must not stop the lift."""
    service = _service(fake_bot, db, fake_clock)
    await service.begin(CHAT_ID, TARGET_ID, timeout_seconds=120)
    fake_bot.fail = {"delete_message"}
    answer = await service.verify(CaptchaCallback(CHAT_ID, TARGET_ID, "deadbeef"), TARGET_ID)
    assert answer == "Verified. Welcome!"
    assert await db.captcha_get(CHAT_ID, TARGET_ID) is None


async def test_sweep_survives_missing_prompt_message(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """The kick must complete even when the prompt is already gone."""
    service = _service(fake_bot, db, fake_clock)
    await service.begin(CHAT_ID, TARGET_ID, timeout_seconds=120)
    fake_bot.fail = {"delete_message"}
    fake_clock.advance(121)
    assert await service.sweep_expired() == 1
    assert fake_bot.called("ban_chat_member")
    assert await db.captcha_get(CHAT_ID, TARGET_ID) is None
