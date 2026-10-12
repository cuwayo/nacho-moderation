"""Extra unit tests for the admin cache and permission resolution.

These complement ``test_admin_cache.py``/``test_perms.py`` by covering the
branches they leave untouched: the single-flight failure path, every ``_rights_of``
flag, ``clear()``, and ``Subject.has`` edge cases.
"""

from __future__ import annotations

import asyncio
from typing import Any

import pytest

from nacho_moderator.clock import FakeClock
from nacho_moderator.constants import Right, Role
from nacho_moderator.services.perms import AdminCache, Subject
from tests.conftest import ADMIN_ID, CHAT_ID, FakeBot, fake_admin


async def test_fetch_ignores_unknown_member_status(
    fake_bot: FakeBot, fake_clock: FakeClock
) -> None:
    """A member whose status is neither creator nor administrator is skipped."""
    fake_bot.admins = [fake_admin(99, status="member")]
    cache = AdminCache(bot=fake_bot, clock=fake_clock)  # type: ignore[arg-type]
    subjects = await cache.get(CHAT_ID)
    assert subjects == {}


async def test_failed_fetch_is_not_cached_and_retried(fake_clock: FakeClock) -> None:
    """A fetch that raises leaves no cached entry, so the next caller retries."""

    class _FlakyBot:
        def __init__(self) -> None:
            self.calls = 0

        async def get_chat_administrators(self, chat_id: int) -> list[Any]:
            self.calls += 1
            if self.calls == 1:
                raise RuntimeError("boom")
            return [fake_admin(ADMIN_ID)]

    bot = _FlakyBot()
    cache = AdminCache(bot=bot, clock=fake_clock)  # type: ignore[arg-type]
    with pytest.raises(RuntimeError, match="boom"):
        await cache.get(CHAT_ID)
    # The failed fetch must not be cached: the next lookup starts a fresh one.
    subjects = await cache.get(CHAT_ID)
    assert ADMIN_ID in subjects
    assert bot.calls == 2


async def test_failed_fetch_after_invalidate_does_not_drop_a_new_entry(
    fake_clock: FakeClock,
) -> None:
    """A fetch that fails after invalidate() must not clobber a later entry."""
    release = asyncio.Event()

    class _SlowFailBot:
        async def get_chat_administrators(self, chat_id: int) -> list[Any]:
            await release.wait()
            raise RuntimeError("boom")

    cache = AdminCache(bot=_SlowFailBot(), clock=fake_clock)  # type: ignore[arg-type]
    first = asyncio.create_task(cache.get(CHAT_ID))
    await asyncio.sleep(0)  # let the fetch start and register as in-flight
    cache.invalidate(CHAT_ID)  # drops the in-flight entry
    release.set()
    with pytest.raises(RuntimeError, match="boom"):
        await first


async def test_clear_drops_cache_and_inflight(fake_bot: FakeBot, fake_clock: FakeClock) -> None:
    fake_bot.admins = [fake_admin(ADMIN_ID)]
    cache = AdminCache(bot=fake_bot, clock=fake_clock)  # type: ignore[arg-type]
    await cache.get(CHAT_ID)
    cache.clear()
    await cache.get(CHAT_ID)
    assert len(fake_bot.called("get_chat_administrators")) == 2


@pytest.mark.parametrize(
    ("flag", "expected"),
    [
        ("can_restrict_members", Right.RESTRICT),
        ("can_delete_messages", Right.DELETE),
        ("can_pin_messages", Right.PIN),
        ("can_promote_members", Right.PROMOTE),
        ("can_change_info", Right.CHANGE_INFO),
        ("", Right.NONE),
    ],
)
async def test_rights_map_each_flag(
    fake_bot: FakeBot,
    fake_clock: FakeClock,
    flag: str,
    expected: Right,
) -> None:
    """Each ``can_*`` flag maps onto exactly its ``Right`` bit."""
    admin = fake_admin(ADMIN_ID, rights="none")
    if flag:
        setattr(admin, flag, True)
    fake_bot.admins = [admin]
    cache = AdminCache(bot=fake_bot, clock=fake_clock)  # type: ignore[arg-type]
    subjects = await cache.get(CHAT_ID)
    assert subjects[ADMIN_ID].rights == expected


def test_subject_owner_holds_every_right() -> None:
    owner = Subject(1, Role.OWNER)
    assert owner.has(Right.RESTRICT)
    assert owner.has(Right.PROMOTE | Right.CHANGE_INFO)


def test_subject_none_requires_nothing() -> None:
    assert Subject(1, Role.ANYONE).has(Right.NONE)


def test_subject_partial_rights_are_not_satisfied() -> None:
    subject = Subject(1, Role.ADMIN, Right.RESTRICT | Right.DELETE)
    assert subject.has(Right.RESTRICT)
    assert subject.has(Right.RESTRICT | Right.DELETE)
    assert not subject.has(Right.RESTRICT | Right.PIN)
