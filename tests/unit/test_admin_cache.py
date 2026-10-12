"""Tests for the administrator cache and its TTL/invalidation behaviour."""

from __future__ import annotations

import asyncio

from nacho_moderator.clock import FakeClock
from nacho_moderator.constants import Right, Role
from nacho_moderator.services.perms import AdminCache, Subject, subject_for_user
from tests.conftest import ADMIN_ID, BOT_ID, CHAT_ID, FakeBot, fake_admin


async def test_fetch_maps_roles_and_rights(fake_bot: FakeBot, fake_clock: FakeClock) -> None:
    fake_bot.admins = [
        fake_admin(100, status="creator"),
        fake_admin(ADMIN_ID, status="administrator"),
    ]
    cache = AdminCache(bot=fake_bot, clock=fake_clock)  # type: ignore[arg-type]
    subjects = await cache.get(CHAT_ID)
    assert subjects[100].role is Role.OWNER
    assert subjects[ADMIN_ID].role is Role.ADMIN
    assert subjects[ADMIN_ID].has(Right.RESTRICT)


async def test_rights_reflect_missing_flags(fake_bot: FakeBot, fake_clock: FakeClock) -> None:
    fake_bot.admins = [fake_admin(ADMIN_ID, can_restrict_members=False)]
    cache = AdminCache(bot=fake_bot, clock=fake_clock)  # type: ignore[arg-type]
    subjects = await cache.get(CHAT_ID)
    assert not subjects[ADMIN_ID].has(Right.RESTRICT)
    assert subjects[ADMIN_ID].has(Right.DELETE)


async def test_cache_hit_avoids_refetch(fake_bot: FakeBot, fake_clock: FakeClock) -> None:
    fake_bot.admins = [fake_admin(ADMIN_ID)]
    cache = AdminCache(bot=fake_bot, clock=fake_clock)  # type: ignore[arg-type]
    await cache.get(CHAT_ID)
    await cache.get(CHAT_ID)
    assert len(fake_bot.called("get_chat_administrators")) == 1


async def test_cache_expires_after_ttl(fake_bot: FakeBot, fake_clock: FakeClock) -> None:
    fake_bot.admins = [fake_admin(ADMIN_ID)]
    cache = AdminCache(bot=fake_bot, clock=fake_clock)  # type: ignore[arg-type]
    await cache.get(CHAT_ID)
    fake_clock.advance(601)  # past the 10-minute TTL
    await cache.get(CHAT_ID)
    assert len(fake_bot.called("get_chat_administrators")) == 2


async def test_invalidate_forces_refetch(fake_bot: FakeBot, fake_clock: FakeClock) -> None:
    fake_bot.admins = [fake_admin(ADMIN_ID)]
    cache = AdminCache(bot=fake_bot, clock=fake_clock)  # type: ignore[arg-type]
    await cache.get(CHAT_ID)
    cache.invalidate(CHAT_ID)
    await cache.get(CHAT_ID)
    assert len(fake_bot.called("get_chat_administrators")) == 2


def test_subject_owner_holds_all_rights() -> None:
    subject = subject_for_user(1, {1: Subject(1, Role.OWNER)}, set())
    assert subject.has(Right.RESTRICT | Right.DELETE | Right.PIN | Right.PROMOTE)


def test_subject_sudo_flag() -> None:
    subject = subject_for_user(BOT_ID, {}, {BOT_ID})
    assert subject.is_sudo
    assert subject.role is Role.SUDO


async def test_concurrent_lookups_share_one_fetch(fake_clock: FakeClock) -> None:
    """A burst of messages for the same chat must not stampede the API."""
    started = asyncio.Event()
    release = asyncio.Event()
    calls: list[int] = []

    class SlowBot:
        async def get_chat_administrators(self, chat_id: int) -> list:
            calls.append(chat_id)
            started.set()
            await release.wait()
            return [fake_admin(ADMIN_ID)]

    cache = AdminCache(bot=SlowBot(), clock=fake_clock)  # type: ignore[arg-type]
    tasks = [asyncio.create_task(cache.get(CHAT_ID)) for _ in range(5)]
    await started.wait()
    release.set()
    results = await asyncio.gather(*tasks)
    assert len(calls) == 1  # one shared fetch, not five
    assert all(result == results[0] for result in results)


async def test_invalidate_during_fetch_does_not_cache_stale(fake_clock: FakeClock) -> None:
    """An invalidate() mid-fetch must not let the old result repopulate the cache."""
    release = asyncio.Event()
    calls: list[int] = []

    class SlowBot:
        async def get_chat_administrators(self, chat_id: int) -> list:
            calls.append(chat_id)
            if len(calls) == 1:
                await release.wait()
            return [fake_admin(ADMIN_ID)]

    cache = AdminCache(bot=SlowBot(), clock=fake_clock)  # type: ignore[arg-type]
    first = asyncio.create_task(cache.get(CHAT_ID))
    await asyncio.sleep(0)  # let the first fetch start
    cache.invalidate(CHAT_ID)
    release.set()
    await first
    # The stale in-flight result must not have been cached.
    await cache.get(CHAT_ID)
    assert len(calls) == 2
