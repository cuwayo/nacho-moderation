"""Token-bucket rate limiting.

Three buckets mirror the original bot's limits: a global bucket (25 req/s), a
per-chat message bucket (15 req/min) and a per-chat bulk bucket (1 req/s). A
request must be admitted by *both* the global bucket and the bucket for its
class. Excess requests wait; they are never dropped. A 429 response pauses the
relevant per-chat bucket (or the global one) for ``retry_after`` seconds.

The bucket arithmetic is pure (driven by the injected clock); the middleware
performs the actual :func:`asyncio.sleep`, so the logic is unit-testable.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from nacho_moderator.clock import Clock
from nacho_moderator.constants import (
    GLOBAL_RATE_PER_SECOND,
    PER_CHAT_BULK_RATE_PER_SECOND,
    PER_CHAT_MESSAGE_RATE_PER_MINUTE,
    Bucket,
)

_CHAT_CAPACITY = 256


@dataclass
class TokenBucket:
    """A single token bucket with a refill rate and an optional pause."""

    capacity: float
    rate_per_second: float
    tokens: float = field(init=False)
    last_refill: float = field(init=False)
    paused_until: float = 0.0

    def __post_init__(self) -> None:
        self.tokens = self.capacity
        self.last_refill = 0.0

    def _refill(self, now: float) -> None:
        elapsed = now - self.last_refill
        if elapsed > 0:
            self.tokens = min(self.capacity, self.tokens + elapsed * self.rate_per_second)
            self.last_refill = now

    def wait_time(self, now: float) -> float:
        """Return seconds until one token is available (0 if available now)."""
        self._refill(now)
        if self.paused_until > now:
            return self.paused_until - now
        if self.tokens >= 1.0:
            return 0.0
        return (1.0 - self.tokens) / self.rate_per_second

    def consume(self, now: float) -> None:
        """Remove one token (call only when :meth:`wait_time` returned 0)."""
        self._refill(now)
        self.tokens = max(0.0, self.tokens - 1.0)

    def pause(self, now: float, seconds: float) -> None:
        """Hold the bucket until ``now + seconds``."""
        self.paused_until = max(self.paused_until, now + seconds)


@dataclass
class _ChatBuckets:
    """The per-chat message and bulk buckets for one chat."""

    message: TokenBucket
    bulk: TokenBucket


class RateLimiter:
    """Global and per-chat token buckets with 429 pause support."""

    def __init__(self, clock: Clock, chat_capacity: int = _CHAT_CAPACITY) -> None:
        self._clock = clock
        self._chat_capacity = chat_capacity
        self._global = TokenBucket(GLOBAL_RATE_PER_SECOND, GLOBAL_RATE_PER_SECOND)
        self._chats: dict[int, _ChatBuckets] = {}

    def _buckets_for(self, chat_id: int | None) -> tuple[TokenBucket, _ChatBuckets | None]:
        """Return the global bucket and the per-chat buckets for a chat."""
        if chat_id is None:
            return self._global, None
        buckets = self._chats.get(chat_id)
        if buckets is None:
            if len(self._chats) >= self._chat_capacity:
                # Evict an arbitrary entry to bound memory (dict preserves order).
                self._chats.pop(next(iter(self._chats)))
            msg_rate = PER_CHAT_MESSAGE_RATE_PER_MINUTE
            buckets = _ChatBuckets(
                message=TokenBucket(msg_rate, msg_rate / 60.0),
                bulk=TokenBucket(PER_CHAT_BULK_RATE_PER_SECOND, PER_CHAT_BULK_RATE_PER_SECOND),
            )
            self._chats[chat_id] = buckets
        return self._global, buckets

    def _pick(self, chat_id: int | None, bucket: Bucket) -> TokenBucket | None:
        _, chat = self._buckets_for(chat_id)
        if chat is None:
            return None
        if bucket is Bucket.BULK:
            return chat.bulk
        if bucket is Bucket.PER_CHAT:
            return chat.message
        return None

    def wait_time(self, chat_id: int | None, bucket: Bucket) -> float:
        """Return seconds to wait before a request may be sent (0 = now)."""
        now = self._clock.monotonic()
        wait = self._global.wait_time(now)
        per_chat = self._pick(chat_id, bucket)
        if per_chat is not None:
            wait = max(wait, per_chat.wait_time(now))
        return wait

    def consume(self, chat_id: int | None, bucket: Bucket) -> None:
        """Consume one token from the global and per-chat buckets."""
        now = self._clock.monotonic()
        self._global.consume(now)
        per_chat = self._pick(chat_id, bucket)
        if per_chat is not None:
            per_chat.consume(now)

    def pause(self, chat_id: int | None, bucket: Bucket, seconds: float) -> None:
        """Pause the relevant bucket(s) for ``seconds`` after a 429."""
        now = self._clock.monotonic()
        per_chat = self._pick(chat_id, bucket)
        if per_chat is not None:
            per_chat.pause(now, seconds)
        else:
            self._global.pause(now, seconds)
