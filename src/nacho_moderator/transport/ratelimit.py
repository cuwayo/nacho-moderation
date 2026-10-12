"""The aiogram client-session middleware that applies rate limiting and retries.

Registered on ``bot.session.middleware(...)``, it wraps every outgoing Bot API
call: it waits for a token in the global and per-chat buckets, then invokes the
wrapped request. On HTTP 429 it pauses the relevant bucket for ``retry_after``
and retries; on a 5xx or network error it retries idempotent methods with the
exponential backoff schedule.
"""

from __future__ import annotations

import asyncio

from aiogram import Bot
from aiogram.client.session.middlewares.base import (
    BaseRequestMiddleware,
    NextRequestMiddlewareType,
)
from aiogram.exceptions import TelegramNetworkError, TelegramRetryAfter, TelegramServerError
from aiogram.methods import Response, TelegramMethod
from aiogram.methods.base import TelegramType

from nacho_moderator.logging_setup import get_logger
from nacho_moderator.transport.buckets import RateLimiter
from nacho_moderator.transport.retry import backoff_seconds, bucket_for, has_budget, is_idempotent

_log = get_logger("ratelimit")

#: Upper bound on 429 retries for one call, so a persistently throttled method
#: eventually surfaces the error instead of looping forever.
MAX_429_RETRIES = 20


class RateLimitMiddleware(BaseRequestMiddleware):
    """Rate-limit and retry middleware for the Bot API session."""

    def __init__(self, limiter: RateLimiter) -> None:
        self._limiter = limiter

    async def __call__(
        self,
        make_request: NextRequestMiddlewareType[TelegramType],
        bot: Bot,
        method: TelegramMethod[TelegramType],
    ) -> Response[TelegramType]:
        """Rate-limit, send, and retry one Bot API method call."""
        name = method.__api_method__
        bucket = bucket_for(name)
        chat_id = getattr(method, "chat_id", None)
        chat_key = int(chat_id) if isinstance(chat_id, int) else None

        attempt = 0
        throttled = 0
        while True:
            wait = self._limiter.wait_time(chat_key, bucket)
            if wait > 0:
                await asyncio.sleep(wait)
            self._limiter.consume(chat_key, bucket)
            try:
                return await make_request(bot, method)
            except TelegramRetryAfter as exc:
                if throttled >= MAX_429_RETRIES:
                    _log.error("%s still throttled after %d retries; giving up", name, throttled)
                    raise
                throttled += 1
                self._limiter.pause(chat_key, bucket, exc.retry_after)
                _log.info("429 on %s; pausing bucket for %ss", name, exc.retry_after)
                continue
            except (TelegramServerError, TelegramNetworkError) as exc:
                if not is_idempotent(name) or not has_budget(attempt):
                    raise
                delay = backoff_seconds(attempt)
                _log.warning("retrying %s after %s (attempt %d): %s", name, delay, attempt + 1, exc)
                await asyncio.sleep(delay)
                attempt += 1


def install_rate_limiter(bot: Bot, limiter: RateLimiter) -> None:
    """Register the rate-limit middleware on a bot's session."""
    bot.session.middleware(RateLimitMiddleware(limiter))


__all__ = ["RateLimitMiddleware", "install_rate_limiter"]
