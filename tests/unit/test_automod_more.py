"""Extra unit tests for the automod pipeline.

These fill the gaps left by ``test_automod.py``: every lock-classification
branch, the regex compile/guard failures, the non-matching substring fall-through,
the antiflood slot cap, and the empty-text short circuit.
"""

from __future__ import annotations

import pytest

from nacho_moderator.clock import FakeClock
from nacho_moderator.constants import FLOOD_SLOTS, LockType
from nacho_moderator.db import Database
from nacho_moderator.services.automod import AutomodService, Verdict


def _bit(*locks: LockType) -> int:
    value = 0
    for lock in locks:
        value |= 1 << int(lock)
    return value


_ALL_LOCKS = _bit(*LockType)


@pytest.fixture
async def automod(db: Database, fake_clock: FakeClock) -> AutomodService:
    return AutomodService(db=db, clock=fake_clock)


@pytest.mark.parametrize(
    ("lock", "kwargs", "text"),
    [
        (LockType.STICKERS, {"has_sticker": True}, ""),
        (LockType.FORWARDS, {"has_forward": True}, ""),
        (LockType.POLLS, {"has_poll": True}, ""),
        (LockType.BOTS, {"from_bot": True}, ""),
        (LockType.MEDIA, {"has_media": True}, ""),
    ],
)
def test_locked_content_flags(lock: LockType, kwargs: dict[str, bool], text: str) -> None:
    """Each content flag maps onto its lock type when that lock is active."""
    hits = AutomodService.locked_content(
        _ALL_LOCKS,
        text=text,
        has_media=kwargs.get("has_media", False),
        has_sticker=kwargs.get("has_sticker", False),
        has_forward=kwargs.get("has_forward", False),
        has_poll=kwargs.get("has_poll", False),
        from_bot=kwargs.get("from_bot", False),
        entity_types=set(),
    )
    assert hits == {lock}


def test_bot_command_entity_triggers_commands_lock() -> None:
    hits = AutomodService.locked_content(
        _bit(LockType.COMMANDS),
        text="/start",
        has_media=False,
        has_sticker=False,
        has_forward=False,
        has_poll=False,
        from_bot=False,
        entity_types={"bot_command"},
    )
    assert hits == {LockType.COMMANDS}


def test_mention_entity_triggers_links_lock() -> None:
    hits = AutomodService.locked_content(
        _bit(LockType.LINKS),
        text="@someone hi",
        has_media=False,
        has_sticker=False,
        has_forward=False,
        has_poll=False,
        from_bot=False,
        entity_types={"mention"},
    )
    assert hits == {LockType.LINKS}


async def test_blacklist_cache_hit_avoids_refetch(automod: AutomodService, db: Database) -> None:
    await db.blacklist_add(-100, "badword", is_regex=False)
    automod.invalidate_blacklist(-100)
    first = await automod.scan(
        chat_id=-100,
        user_id=5,
        message_id=1,
        text="badword",
        locks=0,
        flood_limit=0,
        flood_window=10,
        is_bot=False,
    )
    second = await automod.scan(
        chat_id=-100,
        user_id=5,
        message_id=2,
        text="BADWORD",
        locks=0,
        flood_limit=0,
        flood_window=10,
        is_bot=False,
    )
    assert first.verdict is Verdict.DELETE
    assert second.verdict is Verdict.DELETE


async def test_blacklist_non_matching_substring_falls_through(
    automod: AutomodService, db: Database
) -> None:
    """A substring entry that does not match must not stop the scan early."""
    await db.blacklist_add(-100, "zzz", is_regex=False)
    automod.invalidate_blacklist(-100)
    result = await automod.scan(
        chat_id=-100,
        user_id=5,
        message_id=1,
        text="nothing here",
        locks=0,
        flood_limit=0,
        flood_window=10,
        is_bot=False,
    )
    assert result.verdict is Verdict.ALLOW


async def test_blacklist_invalid_regex_is_skipped(automod: AutomodService, db: Database) -> None:
    """An unparseable pattern is caught and never matches."""
    await db.blacklist_add(-100, "(", is_regex=True)
    automod.invalidate_blacklist(-100)
    result = await automod.scan(
        chat_id=-100,
        user_id=5,
        message_id=1,
        text="anything",
        locks=0,
        flood_limit=0,
        flood_window=10,
        is_bot=False,
    )
    assert result.verdict is Verdict.ALLOW


async def test_empty_text_skips_blacklist(automod: AutomodService, db: Database) -> None:
    await db.blacklist_add(-100, "badword", is_regex=False)
    automod.invalidate_blacklist(-100)
    result = await automod.scan(
        chat_id=-100,
        user_id=5,
        message_id=1,
        text="",
        locks=0,
        flood_limit=0,
        flood_window=10,
        is_bot=False,
    )
    assert result.verdict is Verdict.ALLOW


async def test_antiflood_window_caps_stored_slots(
    automod: AutomodService, fake_clock: FakeClock
) -> None:
    """Once the window holds FLOOD_SLOTS entries the oldest is dropped."""
    for i in range(FLOOD_SLOTS + 1):
        result = await automod.scan(
            chat_id=-100,
            user_id=5,
            message_id=i,
            text="hi",
            locks=0,
            flood_limit=10_000,
            flood_window=10,
            is_bot=False,
        )
        assert result.verdict is Verdict.ALLOW
