"""A small in-memory TTL cache.

Used for the per-chat administrator list, the blacklist, and the antiflood
sliding windows. Entries expire lazily on access; the injected clock drives
expiry so tests are deterministic.
"""

from __future__ import annotations

from dataclasses import dataclass

from nacho_moderator.clock import Clock


@dataclass
class _Entry[V]:
    value: V
    expires_at: float


class TtlCache[V]:
    """A bounded mapping whose values expire after a fixed TTL."""

    def __init__(self, clock: Clock, ttl_seconds: float, capacity: int = 256) -> None:
        self._clock = clock
        self._ttl = ttl_seconds
        self._capacity = capacity
        self._data: dict[str, _Entry[V]] = {}

    def get(self, key: str) -> V | None:
        """Return the live value for ``key``, or ``None`` if absent/expired."""
        entry = self._data.get(key)
        if entry is None:
            return None
        if entry.expires_at <= self._clock.monotonic():
            del self._data[key]
            return None
        return entry.value

    def set(self, key: str, value: V) -> None:
        """Store ``value`` under ``key`` with a fresh TTL."""
        if key not in self._data and len(self._data) >= self._capacity:
            # Evict the entry that expires soonest (simple bounded cache policy).
            oldest = min(self._data, key=lambda k: self._data[k].expires_at)
            del self._data[oldest]
        self._data[key] = _Entry(value, self._clock.monotonic() + self._ttl)

    def invalidate(self, key: str) -> None:
        """Drop ``key`` if present."""
        self._data.pop(key, None)

    def clear(self) -> None:
        """Drop every entry."""
        self._data.clear()
