"""Webhook mode: an aiohttp app behind a TLS-terminating reverse proxy.

Telegram posts updates to a plain-HTTP listener; the reverse proxy terminates
TLS. Every request must carry an ``X-Telegram-Bot-Api-Secret-Token`` header that
matches ``webhook.secret``; a mismatch is rejected with **403** (aiogram's stock
handler returns 401, so we override ``handle``), and a non-POST request gets
**405** automatically because the route is registered for POST only.
"""

from __future__ import annotations

from aiogram import Bot, Dispatcher
from aiogram.webhook.aiohttp_server import SimpleRequestHandler, setup_application
from aiohttp import web

from nacho_moderator.config import Settings
from nacho_moderator.logging_setup import get_logger

_log = get_logger("webhook")

WEBHOOK_PATH = "/webhook"


class Secret403Handler(SimpleRequestHandler):
    """A request handler that answers a bad secret with 403 instead of 401."""

    def verify_secret(self, telegram_secret_token: str, bot: Bot) -> bool:
        """Reject every request when no secret is configured.

        aiogram's base implementation accepts any token when ``secret_token`` is
        falsy, which would let anyone reaching the port forge updates. Config
        validation already forbids an empty secret in webhook mode; this makes
        the handler safe on its own.
        """
        if not self.secret_token:
            return False
        return super().verify_secret(telegram_secret_token, bot)

    async def handle(self, request: web.Request) -> web.Response:
        """Verify the secret header, returning 403 on mismatch."""
        token = request.headers.get("X-Telegram-Bot-Api-Secret-Token", "")
        if not self.verify_secret(token, self.bot):
            return web.Response(body="Forbidden", status=403)
        return await self._handle_request(self.bot, request)


def build_webhook_app(config: Settings, dp: Dispatcher, bot: Bot) -> web.Application:
    """Build the aiohttp application that serves webhook updates."""
    app = web.Application()
    handler = Secret403Handler(dispatcher=dp, bot=bot, secret_token=config.webhook.secret)
    handler.register(app, path=WEBHOOK_PATH)
    setup_application(app, dp, bot=bot)
    return app
