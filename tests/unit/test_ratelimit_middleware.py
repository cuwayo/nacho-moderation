"""Tests for the rate-limit/retry session middleware.

The middleware wraps an outgoing Bot API call; these tests drive it directly with
fake ``make_request`` coroutines and fake Telegram method objects, so no network
or aiogram session is involved.
"""

from __future__ import annotations

from typing import Any, cast

import pytest
from aiogram.exceptions import TelegramNetworkError, TelegramRetryAfter, TelegramServerError
from aiogram.methods import TelegramMethod

from nacho_moderator.clock import FakeClock
from nacho_moderator.constants import Bucket
from nacho_moderator.transport.buckets import RateLimiter
from nacho_moderator.transport.ratelimit import RateLimitMiddleware


class _Method:
    """A minimal stand-in for a Telegram method."""

    def __init__(self, api_method: str, chat_id: int | None = None) -> None:
        self.__api_method__ = api_method
        self.chat_id = chat_id


class _Response:
    """A minimal successful response."""


def _middleware() -> tuple[RateLimitMiddleware, RateLimiter]:
    limiter = RateLimiter(FakeClock())
    return RateLimitMiddleware(limiter), limiter


async def _call(
    mw: RateLimitMiddleware, make_request: Any, method: _Method, *, bot: Any = None
) -> Any:
    """Invoke the middleware with our fakes, cast to the typed signatures."""
    return await mw(make_request, bot=cast("Any", bot), method=cast("TelegramMethod[Any]", method))


async def test_successful_call_passes_through() -> None:
    mw, _ = _middleware()
    seen: list[str] = []

    async def make_request(bot: Any, method: Any) -> Any:
        seen.append(method.__api_method__)
        return _Response()

    result = await _call(mw, make_request, _Method("getMe"))
    assert isinstance(result, _Response)
    assert seen == ["getMe"]


async def test_429_pauses_bucket_and_retries(monkeypatch: pytest.MonkeyPatch) -> None:
    mw, _limiter = _middleware()
    attempts: list[int] = []

    async def make_request(bot: Any, method: Any) -> Any:
        attempts.append(1)
        if len(attempts) == 1:
            raise TelegramRetryAfter(method=method, message="slow down", retry_after=1)
        return _Response()

    sleeps: list[float] = []

    async def fake_sleep(seconds: float) -> None:
        sleeps.append(seconds)

    monkeypatch.setattr("nacho_moderator.transport.ratelimit.asyncio.sleep", fake_sleep)
    result = await _call(mw, make_request, _Method("sendMessage", chat_id=5))
    assert isinstance(result, _Response)
    assert len(attempts) == 2
    # The first sleep is the 429 pause.
    assert sleeps and sleeps[0] >= 1


async def test_server_error_retries_idempotent_method(monkeypatch: pytest.MonkeyPatch) -> None:
    mw, _ = _middleware()
    attempts: list[int] = []

    async def make_request(bot: Any, method: Any) -> Any:
        attempts.append(1)
        if len(attempts) == 1:
            raise TelegramServerError(method=method, message="bad gateway")
        return _Response()

    async def fake_sleep(seconds: float) -> None:
        return None

    monkeypatch.setattr("nacho_moderator.transport.ratelimit.asyncio.sleep", fake_sleep)
    await _call(mw, make_request, _Method("banChatMember", chat_id=5))
    assert len(attempts) == 2


async def test_server_error_does_not_retry_non_idempotent(monkeypatch: pytest.MonkeyPatch) -> None:
    mw, _ = _middleware()
    attempts: list[int] = []

    async def make_request(bot: Any, method: Any) -> Any:
        attempts.append(1)
        raise TelegramServerError(method=method, message="bad gateway")

    async def fake_sleep(seconds: float) -> None:
        return None

    monkeypatch.setattr("nacho_moderator.transport.ratelimit.asyncio.sleep", fake_sleep)
    with pytest.raises(TelegramServerError):
        await _call(mw, make_request, _Method("sendMessage", chat_id=5))
    assert len(attempts) == 1  # sendMessage is not idempotent; no retry


async def test_network_error_retries_up_to_budget(monkeypatch: pytest.MonkeyPatch) -> None:
    mw, _ = _middleware()
    attempts: list[int] = []

    async def make_request(bot: Any, method: Any) -> Any:
        attempts.append(1)
        raise TelegramNetworkError(method=method, message="reset")

    async def fake_sleep(seconds: float) -> None:
        return None

    monkeypatch.setattr("nacho_moderator.transport.ratelimit.asyncio.sleep", fake_sleep)
    with pytest.raises(TelegramNetworkError):
        await _call(mw, make_request, _Method("getMe"))
    assert len(attempts) == 6  # initial attempt + 5 retries


async def test_429_gives_up_after_max_retries(monkeypatch: pytest.MonkeyPatch) -> None:
    mw, _ = _middleware()
    attempts: list[int] = []

    async def make_request(bot: Any, method: Any) -> Any:
        attempts.append(1)
        raise TelegramRetryAfter(method=method, message="slow down", retry_after=0)

    async def fake_sleep(seconds: float) -> None:
        return None

    monkeypatch.setattr("nacho_moderator.transport.ratelimit.asyncio.sleep", fake_sleep)
    with pytest.raises(TelegramRetryAfter):
        await _call(mw, make_request, _Method("getChatAdministrators", chat_id=5))
    from nacho_moderator.transport.ratelimit import MAX_429_RETRIES

    assert len(attempts) == MAX_429_RETRIES + 1


async def test_global_bucket_waits_when_exhausted(monkeypatch: pytest.MonkeyPatch) -> None:
    limiter = RateLimiter(FakeClock())
    mw = RateLimitMiddleware(limiter)
    for _ in range(25):
        limiter.consume(None, Bucket.GLOBAL)
    sleeps: list[float] = []

    async def fake_sleep(seconds: float) -> None:
        sleeps.append(seconds)

    async def make_request(bot: Any, method: Any) -> Any:
        return _Response()

    monkeypatch.setattr("nacho_moderator.transport.ratelimit.asyncio.sleep", fake_sleep)
    await _call(mw, make_request, _Method("getMe"))
    assert sleeps and sleeps[0] > 0
