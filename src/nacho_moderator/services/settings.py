"""Per-chat settings service with a short TTL cache.

Reads come from a cache (invalidated on every write) so hot paths like automod
do not hit SQLite for every message. Writes go straight to the database and then
drop the cache entry.
"""

from __future__ import annotations

from dataclasses import dataclass

from nacho_moderator.clock import Clock
from nacho_moderator.db import ChatSettings, Database
from nacho_moderator.services.cache import TtlCache

SETTINGS_TTL_SECONDS = 60.0


@dataclass
class SettingsService:
    """A cache-fronted view of the ``chats`` table."""

    db: Database
    clock: Clock
    _cache: TtlCache[ChatSettings] | None = None

    def __post_init__(self) -> None:
        self._cache = TtlCache(self.clock, SETTINGS_TTL_SECONDS, capacity=512)

    async def get(self, chat_id: int) -> ChatSettings:
        """Return a chat's settings, consulting the cache first."""
        assert self._cache is not None
        key = str(chat_id)
        cached = self._cache.get(key)
        if cached is not None:
            return cached
        settings = await self.db.get_settings(chat_id)
        self._cache.set(key, settings)
        return settings

    async def set(self, chat_id: int, column: str, value: int | str) -> None:
        """Write one setting and invalidate the cached row."""
        await self.db.set_setting(chat_id, column, value)
        self.invalidate(chat_id)

    def invalidate(self, chat_id: int) -> None:
        """Drop the cached settings for a chat."""
        assert self._cache is not None
        self._cache.invalidate(str(chat_id))
