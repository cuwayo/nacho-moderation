"""Injectable time and randomness sources.

Logic modules never call :func:`time.time` or :func:`time.monotonic` directly;
they take a :class:`Clock` so tests can advance time deterministically. The same
applies to random nonce generation for the captcha flow.
"""

from __future__ import annotations

import random
import time
from typing import Protocol, runtime_checkable


@runtime_checkable
class Clock(Protocol):
    """A source of wall-clock and monotonic time."""

    def now(self) -> float:
        """Return the current wall-clock time as a Unix timestamp (seconds)."""
        ...

    def monotonic(self) -> float:
        """Return a monotonic clock value in seconds, for intervals and TTLs."""
        ...


class SystemClock:
    """The real clock, backed by :mod:`time`."""

    def now(self) -> float:
        """Return :func:`time.time`."""
        return time.time()

    def monotonic(self) -> float:
        """Return :func:`time.monotonic`."""
        return time.monotonic()


@runtime_checkable
class RandomSource(Protocol):
    """A source of random values for nonces and identifiers."""

    def token_hex(self, nbytes: int) -> str:
        """Return ``nbytes`` random bytes as a lowercase hex string."""
        ...


class SystemRandom:
    """The real randomness source, backed by :mod:`secrets`."""

    def token_hex(self, nbytes: int) -> str:
        """Return ``nbytes`` cryptographically-random bytes as hex."""
        return random.SystemRandom().getrandbits(nbytes * 8).to_bytes(nbytes, "big").hex()


class FakeClock:
    """A deterministic clock for tests.

    Time only advances when :meth:`advance` or :meth:`set` is called, so
    TTLs, flood windows, and captcha deadlines are fully controllable.
    """

    def __init__(self, start: float = 1_700_000_000.0) -> None:
        self._wall = start
        self._mono = 0.0

    def now(self) -> float:
        """Return the current fake wall-clock time."""
        return self._wall

    def monotonic(self) -> float:
        """Return the current fake monotonic time."""
        return self._mono

    def advance(self, seconds: float) -> None:
        """Move both the wall clock and the monotonic clock forward."""
        self._wall += seconds
        self._mono += seconds

    def set(self, wall: float) -> None:
        """Set the wall clock without disturbing the monotonic clock."""
        self._wall = wall


class FakeRandom:
    """A deterministic randomness source for tests."""

    def __init__(self, value: int = 0) -> None:
        self._value = value

    def token_hex(self, nbytes: int) -> str:
        """Return a fixed hex string derived from the configured value."""
        return format(self._value % (16 ** (nbytes * 2)), f"0{nbytes * 2}x")
