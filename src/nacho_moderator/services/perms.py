"""Administrator cache and permission resolution.

The bot caches ``getChatAdministrators`` per chat for 10 minutes (invalidated on
any ``chat_member``/``my_chat_member`` update and on ``/reload``). From that list
it derives a caller's :class:`~nacho_moderator.constants.Role` and the set of
:class:`~nacho_moderator.constants.Right` bits they hold, and the same for the
bot itself, so the router can gate a command on both.
"""

from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from typing import Any

from aiogram import Bot

from nacho_moderator.clock import Clock
from nacho_moderator.constants import ADMIN_CACHE_TTL_SECONDS, Right, Role
from nacho_moderator.logging_setup import get_logger
from nacho_moderator.services.cache import TtlCache

_log = get_logger("perms")


@dataclass(frozen=True)
class Subject:
    """A caller's or the bot's resolved role and held rights in a chat."""

    user_id: int
    role: Role
    rights: Right = Right.NONE
    is_sudo: bool = False
    username: str | None = None

    def has(self, right: Right) -> bool:
        """Return ``True`` if the subject holds every bit in ``right``."""
        if right is Right.NONE:
            return True
        if self.role is Role.OWNER:
            return True
        return (self.rights & right) == right


@dataclass
class AdminCache:
    """A TTL cache of per-chat administrator lists.

    Concurrent lookups for the same chat share a single in-flight fetch (a
    "single flight"): without it, a burst of messages arriving before the first
    ``getChatAdministrators`` completes would each start their own request and
    trip the Bot API rate limit.
    """

    bot: Bot
    clock: Clock
    _cache: TtlCache[dict[int, Subject]] = field(init=False)
    _inflight: dict[str, asyncio.Task[dict[int, Subject]]] = field(init=False)

    def __post_init__(self) -> None:
        self._cache = TtlCache(self.clock, ADMIN_CACHE_TTL_SECONDS, capacity=256)
        self._inflight = {}

    async def get(self, chat_id: int) -> dict[int, Subject]:
        """Return the admin subjects for a chat, fetching and caching on a miss.

        Concurrent callers for the same chat await one shared fetch.
        """
        key = str(chat_id)
        cached = self._cache.get(key)
        if cached is not None:
            return cached
        task = self._inflight.get(key)
        if task is None:
            task = asyncio.ensure_future(self._fetch(chat_id))
            self._inflight[key] = task
        try:
            subjects = await task
        except BaseException:
            # A failed fetch is never cached, and the next caller retries.
            if self._inflight.get(key) is task:
                del self._inflight[key]
            raise
        # Only the caller that still owns the in-flight entry writes the cache,
        # so an invalidate() that happened mid-fetch is not undone by stale data.
        if self._inflight.get(key) is task:
            del self._inflight[key]
            self._cache.set(key, subjects)
        return subjects

    async def _fetch(self, chat_id: int) -> dict[int, Subject]:
        """Fetch and map the administrator list for a chat."""
        admins = await self.bot.get_chat_administrators(chat_id)
        subjects: dict[int, Subject] = {}
        for member in admins:
            user_id = member.user.id
            username = member.user.username
            status = str(member.status)
            if status == "creator":
                subjects[user_id] = Subject(user_id, Role.OWNER, Right(0), username=username)
            elif status == "administrator":
                subjects[user_id] = Subject(
                    user_id, Role.ADMIN, _rights_of(member), username=username
                )
        return subjects

    def invalidate(self, chat_id: int) -> None:
        """Drop the cached admin list for a chat, and any in-flight fetch.

        Dropping the in-flight entry means a fetch that started before the
        invalidation does not repopulate the cache with stale data; the next
        lookup starts a fresh fetch.
        """
        key = str(chat_id)
        self._cache.invalidate(key)
        self._inflight.pop(key, None)

    def clear(self) -> None:
        """Drop every cached admin list and in-flight fetch."""
        self._cache.clear()
        self._inflight.clear()


def _rights_of(member: Any) -> Right:
    """Map an administrator's ``can_*`` flags onto ``Right`` bits."""
    rights = Right.NONE
    if member.can_restrict_members:
        rights |= Right.RESTRICT
    if member.can_delete_messages:
        rights |= Right.DELETE
    if member.can_pin_messages:
        rights |= Right.PIN
    if member.can_promote_members:
        rights |= Right.PROMOTE
    if member.can_change_info:
        rights |= Right.CHANGE_INFO
    return rights


def subject_for_user(user_id: int, admins: dict[int, Subject], sudo_ids: set[int]) -> Subject:
    """Resolve a caller's subject from the admin list plus the sudo set."""
    is_sudo = user_id in sudo_ids
    admin = admins.get(user_id)
    if admin is not None:
        return Subject(admin.user_id, admin.role, admin.rights, is_sudo, admin.username)
    if is_sudo:
        return Subject(user_id, Role.SUDO, Right.NONE, is_sudo=True)
    return Subject(user_id, Role.ANYONE, Right.NONE, is_sudo=False)
