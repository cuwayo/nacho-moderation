"""Tests for the injectable clock and random sources."""

from __future__ import annotations

import re

from nacho_moderator.clock import (
    Clock,
    FakeClock,
    FakeRandom,
    RandomSource,
    SystemClock,
    SystemRandom,
)


def test_system_clock_now_and_monotonic() -> None:
    clock = SystemClock()
    assert clock.now() > 0
    assert clock.monotonic() >= 0
    assert isinstance(clock, Clock)


def test_system_random_token_hex() -> None:
    source = SystemRandom()
    token = source.token_hex(8)
    assert re.fullmatch(r"[0-9a-f]{16}", token)
    assert isinstance(source, RandomSource)


def test_fake_clock_advance_moves_both() -> None:
    clock = FakeClock(start=100.0)
    assert clock.now() == 100.0
    assert clock.monotonic() == 0.0
    clock.advance(5.0)
    assert clock.now() == 105.0
    assert clock.monotonic() == 5.0


def test_fake_clock_set_only_wall() -> None:
    clock = FakeClock(start=100.0)
    clock.advance(5.0)
    clock.set(200.0)
    assert clock.now() == 200.0
    assert clock.monotonic() == 5.0  # unchanged


def test_fake_random_is_deterministic() -> None:
    assert FakeRandom(255).token_hex(1) == "ff"
    assert FakeRandom(1).token_hex(2) == "0001"
