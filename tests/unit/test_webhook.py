"""Tests for the webhook app construction and the secret-check handler."""

from __future__ import annotations

from typing import Any, cast

from aiogram import Bot, Dispatcher

from nacho_moderator.config import Settings, WebhookSettings
from nacho_moderator.webhook import WEBHOOK_PATH, Secret403Handler, build_webhook_app


def _settings() -> Settings:
    return Settings.model_validate(
        {
            "bot_token": "123456:TEST",
            "mode": "webhook",
            "webhook": {"secret": "0123456789abcdef"},
        }
    )


def test_build_webhook_app_registers_the_route() -> None:
    """build_webhook_app wires the secret handler and the dispatcher lifecycle."""
    from aiogram import Bot, Dispatcher

    bot = Bot(token="123456:TEST")
    try:
        app = build_webhook_app(_settings(), Dispatcher(), bot)
        paths = {
            route.resource.canonical for route in app.router.routes() if route.resource is not None
        }
        assert WEBHOOK_PATH in paths
    finally:
        import asyncio

        asyncio.run(bot.session.close())


async def test_handle_forwards_a_valid_secret() -> None:
    """A valid secret reaches aiogram's request handler."""
    forwarded: list[str] = []

    class _Handler(Secret403Handler):
        async def _handle_request(self, bot: Any, request: Any) -> Any:
            forwarded.append(request.headers.get("X-Telegram-Bot-Api-Secret-Token", ""))
            return type("R", (), {"status": 200})()

    class _Request:
        def __init__(self) -> None:
            self.headers = {"X-Telegram-Bot-Api-Secret-Token": "0123456789abcdef"}

    handler = _Handler(
        dispatcher=cast("Dispatcher", object()),
        bot=cast("Bot", object()),
        secret_token="0123456789abcdef",
    )
    response = await handler.handle(cast("Any", _Request()))
    assert response.status == 200
    assert forwarded == ["0123456789abcdef"]


async def test_handle_rejects_a_bad_secret() -> None:
    class _Request:
        def __init__(self) -> None:
            self.headers = {"X-Telegram-Bot-Api-Secret-Token": "wrong"}

    handler = Secret403Handler(
        dispatcher=cast("Dispatcher", object()),
        bot=cast("Bot", object()),
        secret_token="0123456789abcdef",
    )
    response = await handler.handle(cast("Any", _Request()))
    assert response.status == 403


def test_verify_secret_rejects_when_unconfigured() -> None:
    handler = Secret403Handler(
        dispatcher=cast("Dispatcher", object()),
        bot=cast("Bot", object()),
        secret_token="",
    )
    assert handler.verify_secret("anything", cast("Bot", object())) is False


def test_settings_webhook_defaults() -> None:
    settings = WebhookSettings()
    assert settings.listen == "127.0.0.1:8080"
    assert settings.secret == ""
