"""Tests for the periodic background task and the webhook secret handling."""

from __future__ import annotations

import asyncio

from aiohttp import web

from nacho_moderator.services.periodic import PeriodicTask
from nacho_moderator.webhook import Secret403Handler


class _CountingTask(PeriodicTask):
    def __init__(self, interval: float) -> None:
        super().__init__(interval, "test")
        self.ticks = 0

    async def _tick(self) -> None:
        self.ticks += 1


async def test_periodic_task_ticks_and_stops() -> None:
    task = _CountingTask(interval=0.01)
    task.start()
    await asyncio.sleep(0.05)
    await task.stop()
    assert task.ticks >= 1


async def test_periodic_task_survives_tick_error() -> None:
    class _Boom(PeriodicTask):
        def __init__(self) -> None:
            super().__init__(0.01, "boom")
            self.ticks = 0

        async def _tick(self) -> None:
            self.ticks += 1
            raise RuntimeError("boom")

    task = _Boom()
    task.start()
    await asyncio.sleep(0.05)
    await task.stop()
    assert task.ticks >= 1  # kept running despite the error


async def test_stop_without_start_is_noop() -> None:
    task = _CountingTask(0.01)
    await task.stop()  # must not raise


class _FakeRequest:
    """A minimal aiohttp request stand-in for the secret check."""

    def __init__(self, secret: str | None) -> None:
        self.headers = {"X-Telegram-Bot-Api-Secret-Token": secret} if secret else {}


def _handler(secret: str) -> Secret403Handler:
    # Construct without a real dispatcher/bot; we only exercise verify_secret.
    return Secret403Handler(dispatcher=None, bot=None, secret_token=secret)  # type: ignore[arg-type]


def test_webhook_secret_matches() -> None:
    handler = _handler("0123456789abcdef")
    assert handler.verify_secret("0123456789abcdef", None) is True


def test_webhook_secret_mismatch() -> None:
    handler = _handler("0123456789abcdef")
    assert handler.verify_secret("wrong", None) is False


def test_webhook_secret_empty_configured_rejects_all() -> None:
    handler = _handler("")
    assert handler.verify_secret("anything", None) is False


def test_webhook_returns_403_on_bad_secret(monkeypatch) -> None:
    handler = _handler("0123456789abcdef")
    request = _FakeRequest("wrong")

    async def run() -> web.Response:
        return await handler.handle(request)  # type: ignore[arg-type]

    response = asyncio.run(run())
    assert response.status == 403
