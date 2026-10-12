"""Entry point: load config, assemble the bot, and run polling or webhook mode.

Startup order mirrors the original bot: load and validate config, build the Bot
and Dispatcher, open the database, probe ``getMe``, register the command menu,
then start the chosen update source. Shutdown is graceful: on SIGINT/SIGTERM the
update source stops, in-flight updates are drained for up to 10 seconds, and the
database and session are closed.
"""

from __future__ import annotations

import asyncio
import contextlib
import signal
import sys
from pathlib import Path

from aiogram import Bot, Dispatcher
from aiogram.client.default import DefaultBotProperties
from aiogram.client.session.aiohttp import AiohttpSession
from aiogram.client.telegram import TelegramAPIServer
from aiogram.enums import ParseMode
from aiohttp import web

from nacho_moderator import __version__
from nacho_moderator.clock import SystemClock, SystemRandom
from nacho_moderator.commands.publish import publish_commands
from nacho_moderator.config import (
    LogLevelName,
    Mode,
    Settings,
    load_settings,
    parse_cli,
)
from nacho_moderator.db import Database
from nacho_moderator.errors import ConfigError
from nacho_moderator.handlers import App, build_dispatcher
from nacho_moderator.logging_setup import configure_logging, get_logger
from nacho_moderator.services.automod import AutomodService
from nacho_moderator.services.captcha import CaptchaService
from nacho_moderator.services.expiry import CaptchaSweeper, ExpirySweeper
from nacho_moderator.services.perms import AdminCache
from nacho_moderator.services.settings import SettingsService
from nacho_moderator.transport.buckets import RateLimiter
from nacho_moderator.transport.ratelimit import install_rate_limiter
from nacho_moderator.webhook import build_webhook_app

_log = get_logger("main")

ALLOWED_UPDATES = ["message", "edited_message", "callback_query", "chat_member", "my_chat_member"]
SHUTDOWN_DRAIN_SECONDS = 10.0


def build_bot(config: Settings) -> Bot:
    """Construct the aiogram Bot pointed at the configured API base."""
    server = TelegramAPIServer.from_base(config.api.base)
    session = AiohttpSession(api=server, timeout=config.api.request_timeout_seconds)
    return Bot(
        token=config.bot_token,
        session=session,
        default=DefaultBotProperties(parse_mode=ParseMode.HTML),
    )


async def build_app(config: Settings, bot: Bot) -> App:
    """Assemble the dispatcher and all services."""
    db = await Database.connect(config.db.path)
    clock = SystemClock()
    settings_service = SettingsService(db=db, clock=clock)
    admins = AdminCache(bot=bot, clock=clock)
    automod = AutomodService(db=db, clock=clock)
    captcha = CaptchaService(db=db, clock=clock, random=SystemRandom(), bot=bot)
    expiry = ExpirySweeper(db=db, clock=clock, bot=bot)
    captcha_sweeper = CaptchaSweeper(captcha)

    me = await bot.get_me()
    app = App(
        config=config,
        bot=bot,
        dp=Dispatcher(),
        db=db,
        settings=settings_service,
        admins=admins,
        automod=automod,
        captcha=captcha,
        captcha_sweeper=captcha_sweeper,
        expiry=expiry,
        sudo_ids=set(config.sudo_ids),
        clock=clock,
        bot_username=me.username,
        bot_id=me.id,
    )
    build_dispatcher(app)
    return app


async def _run_polling(app: App, stop: asyncio.Event) -> None:
    """Run long polling with a bounded graceful drain."""
    polling = asyncio.create_task(
        app.dp.start_polling(
            app.bot,
            polling_timeout=app.config.polling.timeout,
            allowed_updates=ALLOWED_UPDATES,
            handle_signals=False,
        )
    )
    await stop.wait()
    _log.info("shutdown requested; draining")
    with contextlib.suppress(Exception):
        await app.dp.stop_polling()
    try:
        await asyncio.wait_for(polling, timeout=SHUTDOWN_DRAIN_SECONDS)
    except (TimeoutError, asyncio.CancelledError):
        polling.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await polling


async def _run_webhook(app: App, stop: asyncio.Event) -> None:
    """Run the webhook listener with a bounded graceful drain."""
    web_app = build_webhook_app(app.config, app.dp, app.bot)
    runner = web.AppRunner(web_app)
    await runner.setup()
    site = web.TCPSite(runner, app.config.webhook.host, app.config.webhook.port)
    await site.start()
    _log.info("webhook listening on %s", app.config.webhook.listen)
    await stop.wait()
    _log.info("shutdown requested; draining")
    await runner.cleanup()


def _install_signal_handlers(stop: asyncio.Event) -> None:
    """Wire SIGINT/SIGTERM to set the stop event."""
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        with contextlib.suppress(NotImplementedError):
            loop.add_signal_handler(sig, stop.set)


async def run_bot(config: Settings) -> int:
    """Run the bot until it is asked to stop."""
    bot = build_bot(config)
    install_rate_limiter(bot, RateLimiter(SystemClock(), chat_capacity=256))
    app = await build_app(config, bot)
    _log.info("getMe ok: id=%s username=%s", app.bot_id, app.bot_username)
    await publish_commands(bot)

    stop = asyncio.Event()
    _install_signal_handlers(stop)
    try:
        if config.mode is Mode.WEBHOOK:
            await _run_webhook(app, stop)
        else:
            await _run_polling(app, stop)
    finally:
        await app.expiry.stop()
        await app.captcha_sweeper.stop()
        await app.db.close()
        await bot.session.close()
        _log.info("stopped")
    return 0


def main(argv: list[str] | None = None) -> int:
    """CLI entry point.

    Returns:
        The process exit code: 0 on success, 1 on a configuration or runtime
        error, 2 on a usage error.
    """
    args, overrides = parse_cli(argv)
    if args.version:
        print(f"nacho-moderator {__version__}")
        return 0

    configure_logging(LogLevelName(overrides.get("log.level", LogLevelName.INFO.value)))

    try:
        if args.check_config is not None:
            load_settings(Path(args.check_config), required_file=True)
            print(f"{args.check_config}: ok")
            return 0
        config = load_settings(
            Path(args.config) if args.config else None,
            required_file=args.config is not None,
            cli_overrides=overrides,
        )
    except ConfigError as exc:
        print(f"nacho-moderator: error: {exc}", file=sys.stderr)
        return 1

    _log.debug("configuration: %s", config.redacted())
    try:
        return asyncio.run(run_bot(config))
    except KeyboardInterrupt:
        return 0
    except Exception as exc:
        _log.error("fatal: %s", exc)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
