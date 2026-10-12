"""Unit tests for the automod pipeline: locks, blacklist, and antiflood."""

from __future__ import annotations

import pytest

from nacho_moderator.clock import FakeClock
from nacho_moderator.constants import LockType
from nacho_moderator.db import Database
from nacho_moderator.services.automod import AutomodService, Verdict


def _bit(*locks: LockType) -> int:
    value = 0
    for lock in locks:
        value |= 1 << int(lock)
    return value


def test_locked_content_matches_only_active_locks() -> None:
    hits = AutomodService.locked_content(
        _bit(LockType.LINKS),
        text="see https://example.com",
        has_media=False,
        has_sticker=False,
        has_forward=False,
        has_poll=False,
        from_bot=False,
        entity_types={"url"},
    )
    assert hits == {LockType.LINKS}


def test_locked_content_ignores_inactive_locks() -> None:
    hits = AutomodService.locked_content(
        0,
        text="see https://example.com",
        has_media=True,
        has_sticker=False,
        has_forward=False,
        has_poll=False,
        from_bot=False,
        entity_types={"url"},
    )
    assert hits == set()


def test_invite_marker_detected() -> None:
    hits = AutomodService.locked_content(
        _bit(LockType.INVITES),
        text="join t.me/joinchat/abc",
        has_media=False,
        has_sticker=False,
        has_forward=False,
        has_poll=False,
        from_bot=False,
        entity_types=set(),
    )
    assert hits == {LockType.INVITES}


@pytest.fixture
async def automod(db: Database, fake_clock: FakeClock) -> AutomodService:
    return AutomodService(db=db, clock=fake_clock)


async def test_blacklist_substring_matches(automod: AutomodService, db: Database) -> None:
    await db.blacklist_add(-100, "badword", is_regex=False)
    automod.invalidate_blacklist(-100)
    result = await automod.scan(
        chat_id=-100,
        user_id=5,
        message_id=1,
        text="this has BADWORD in it",
        locks=0,
        flood_limit=0,
        flood_window=10,
        is_bot=False,
    )
    assert result.verdict is Verdict.DELETE
    assert result.reason == "badword"


async def test_blacklist_regex_matches(automod: AutomodService, db: Database) -> None:
    await db.blacklist_add(-100, r"sp[ae]m\d+", is_regex=True)
    automod.invalidate_blacklist(-100)
    result = await automod.scan(
        chat_id=-100,
        user_id=5,
        message_id=1,
        text="spam123 here",
        locks=0,
        flood_limit=0,
        flood_window=10,
        is_bot=False,
    )
    assert result.verdict is Verdict.DELETE


async def test_blacklist_catastrophic_regex_does_not_match(
    automod: AutomodService, db: Database
) -> None:
    # A pathological pattern must not stall; it simply never matches.
    await db.blacklist_add(-100, r"(a+)+$", is_regex=True)
    automod.invalidate_blacklist(-100)
    result = await automod.scan(
        chat_id=-100,
        user_id=5,
        message_id=1,
        text="a" * 40 + "!",
        locks=0,
        flood_limit=0,
        flood_window=10,
        is_bot=False,
    )
    assert result.verdict is Verdict.ALLOW


async def test_bot_is_skipped_unless_bots_lock(automod: AutomodService) -> None:
    result = await automod.scan(
        chat_id=-100,
        user_id=5,
        message_id=1,
        text="hello",
        locks=0,
        flood_limit=0,
        flood_window=10,
        is_bot=True,
    )
    assert result.verdict is Verdict.ALLOW


async def test_antiflood_trips_strictly_above_limit(
    automod: AutomodService, fake_clock: FakeClock
) -> None:
    # limit=3 -> the 4th message in the window trips.
    for i in range(3):
        result = await automod.scan(
            chat_id=-100,
            user_id=5,
            message_id=i,
            text="hi",
            locks=0,
            flood_limit=3,
            flood_window=10,
            is_bot=False,
        )
        assert result.verdict is Verdict.ALLOW
    result = await automod.scan(
        chat_id=-100,
        user_id=5,
        message_id=3,
        text="hi",
        locks=0,
        flood_limit=3,
        flood_window=10,
        is_bot=False,
    )
    assert result.verdict is Verdict.MUTE
    assert result.mute_seconds == 300


async def test_antiflood_window_expires(automod: AutomodService, fake_clock: FakeClock) -> None:
    for i in range(3):
        await automod.scan(
            chat_id=-100,
            user_id=5,
            message_id=i,
            text="hi",
            locks=0,
            flood_limit=3,
            flood_window=10,
            is_bot=False,
        )
    fake_clock.advance(11)
    result = await automod.scan(
        chat_id=-100,
        user_id=5,
        message_id=9,
        text="hi",
        locks=0,
        flood_limit=3,
        flood_window=10,
        is_bot=False,
    )
    assert result.verdict is Verdict.ALLOW


async def test_antiflood_isolates_users(automod: AutomodService) -> None:
    for i in range(4):
        await automod.scan(
            chat_id=-100,
            user_id=5,
            message_id=i,
            text="hi",
            locks=0,
            flood_limit=3,
            flood_window=10,
            is_bot=False,
        )
    result = await automod.scan(
        chat_id=-100,
        user_id=6,
        message_id=99,
        text="hi",
        locks=0,
        flood_limit=3,
        flood_window=10,
        is_bot=False,
    )
    assert result.verdict is Verdict.ALLOW
