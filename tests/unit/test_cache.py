"""Unit tests for the in-memory TTL cache: expiry, eviction, and invalidation."""

from __future__ import annotations

from nacho_moderator.clock import FakeClock
from nacho_moderator.services.cache import TtlCache


def test_get_returns_none_for_missing_key() -> None:
    cache: TtlCache[int] = TtlCache(FakeClock(), ttl_seconds=10)
    assert cache.get("absent") is None


def test_set_then_get_round_trips() -> None:
    cache: TtlCache[int] = TtlCache(FakeClock(), ttl_seconds=10)
    cache.set("k", 7)
    assert cache.get("k") == 7


def test_entry_expires_after_ttl() -> None:
    clock = FakeClock()
    cache: TtlCache[int] = TtlCache(clock, ttl_seconds=10)
    cache.set("k", 1)
    clock.advance(11)
    assert cache.get("k") is None


def test_expiry_is_inclusive_at_the_boundary() -> None:
    """An entry whose deadline equals "now" is already expired."""
    clock = FakeClock()
    cache: TtlCache[int] = TtlCache(clock, ttl_seconds=10)
    cache.set("k", 1)
    clock.advance(10)
    assert cache.get("k") is None


def test_set_at_capacity_evicts_the_soonest_expiry() -> None:
    clock = FakeClock()
    cache: TtlCache[int] = TtlCache(clock, ttl_seconds=10, capacity=2)
    cache.set("a", 1)
    clock.advance(1)
    cache.set("b", 2)
    clock.advance(1)
    cache.set("c", 3)  # full: "a" expires soonest, so it goes first
    assert cache.get("a") is None
    assert cache.get("b") == 2
    assert cache.get("c") == 3


def test_overwriting_a_live_key_does_not_evict() -> None:
    clock = FakeClock()
    cache: TtlCache[int] = TtlCache(clock, ttl_seconds=10, capacity=1)
    cache.set("a", 1)
    cache.set("a", 2)  # the key is present, so the eviction path is skipped
    assert cache.get("a") == 2


def test_invalidate_drops_the_key() -> None:
    cache: TtlCache[int] = TtlCache(FakeClock(), ttl_seconds=10)
    cache.set("k", 1)
    cache.invalidate("k")
    assert cache.get("k") is None
    cache.invalidate("k")  # an absent key is a no-op


def test_clear_drops_everything() -> None:
    cache: TtlCache[int] = TtlCache(FakeClock(), ttl_seconds=10)
    cache.set("a", 1)
    cache.set("b", 2)
    cache.clear()
    assert cache.get("a") is None
    assert cache.get("b") is None
