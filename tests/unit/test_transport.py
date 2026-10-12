"""Unit tests for the token buckets and retry policy."""

from __future__ import annotations

from nacho_moderator.clock import FakeClock
from nacho_moderator.constants import Bucket
from nacho_moderator.transport.buckets import RateLimiter, TokenBucket
from nacho_moderator.transport.retry import backoff_seconds, bucket_for, has_budget, is_idempotent


def test_token_bucket_refills_over_time() -> None:
    clock = FakeClock()
    bucket = TokenBucket(capacity=2, rate_per_second=1.0)
    bucket.consume(clock.monotonic())
    bucket.consume(clock.monotonic())
    assert bucket.wait_time(clock.monotonic()) > 0
    clock.advance(1.0)
    assert bucket.wait_time(clock.monotonic()) == 0


def test_token_bucket_pause_holds_the_bucket() -> None:
    clock = FakeClock()
    bucket = TokenBucket(capacity=1, rate_per_second=10.0)
    bucket.pause(clock.monotonic(), 5.0)
    assert bucket.wait_time(clock.monotonic()) == 5.0
    clock.advance(5.0)
    assert bucket.wait_time(clock.monotonic()) == 0


def test_global_bucket_limits_to_capacity() -> None:
    clock = FakeClock()
    limiter = RateLimiter(clock)
    for _ in range(25):
        assert limiter.wait_time(None, Bucket.GLOBAL) == 0
        limiter.consume(None, Bucket.GLOBAL)
    assert limiter.wait_time(None, Bucket.GLOBAL) > 0


def test_per_chat_bulk_is_stricter_than_global() -> None:
    clock = FakeClock()
    limiter = RateLimiter(clock)
    assert limiter.wait_time(1, Bucket.BULK) == 0
    limiter.consume(1, Bucket.BULK)
    # The bulk bucket is 1/s, so a second call must wait ~1s.
    assert limiter.wait_time(1, Bucket.BULK) == 1.0


def test_pause_only_affects_the_targeted_chat() -> None:
    clock = FakeClock()
    limiter = RateLimiter(clock)
    limiter.pause(1, Bucket.BULK, 30.0)
    assert limiter.wait_time(1, Bucket.BULK) > 0
    assert limiter.wait_time(2, Bucket.BULK) == 0


def test_method_bucket_and_idempotency_mapping() -> None:
    assert bucket_for("sendMessage") is Bucket.PER_CHAT
    assert bucket_for("banChatMember") is Bucket.BULK
    assert bucket_for("getMe") is Bucket.GLOBAL
    assert is_idempotent("banChatMember") is True
    assert is_idempotent("sendMessage") is False


def test_backoff_schedule_and_budget() -> None:
    assert [backoff_seconds(i) for i in range(7)] == [1, 2, 4, 8, 16, 30, 30]
    assert has_budget(4) is True
    assert has_budget(5) is False
