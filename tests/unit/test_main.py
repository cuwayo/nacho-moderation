"""Tests for the entry point: bot/app assembly, update sources, and the CLI.

The heavy collaborators (the aiogram HTTP session, the polling loop, the webhook
server, and ``asyncio.run``) are monkeypatched, so these tests exercise *our*
startup, shutdown, and branch logic without touching the network or driving the
real aiogram dispatcher.
"""

from __future__ import annotations

import asyncio
import signal
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest
from aiogram import Dispatcher
from aiogram.enums import ParseMode
from aiogram.types import Update
from aiohttp import web

from nacho_moderator import main as main_module
from nacho_moderator.clock import FakeClock
from nacho_moderator.config import Mode, Settings
from nacho_moderator.handlers import App
from nacho_moderator.main import (
    _clear_aiogram_caches,
    _install_metrics_dump,
    _install_signal_handlers,
    _run_polling,
    _run_webhook,
    build_app,
    build_bot,
    main,
    run_bot,
)
from nacho_moderator.metrics import Metrics


def _settings(**overrides: Any) -> Settings:
    """Build a validated :class:`Settings` with test-friendly defaults."""
    data: dict[str, Any] = {
        "bot_token": "123456:TESTTOKEN",
        "mode": "polling",
        "db": {"path": ":memory:"},
    }
    data.update(overrides)
    return Settings.model_validate(data)


def _secure_config(tmp_path: Path) -> Path:
    """Write a minimal, secret-bearing config file with safe permissions."""
    path = tmp_path / "config.toml"
    path.write_text('bot_token = "123456:TESTTOKEN"\nmode = "polling"\n', encoding="utf-8")
    path.chmod(0o600)
    return path


class _Closer:
    """Records the name of each ``close``/``stop`` call into a shared log."""

    def __init__(self, name: str, log: list[str]) -> None:
        self._name = name
        self._log = log

    async def close(self) -> None:
        self._log.append(self._name)

    async def stop(self) -> None:
        self._log.append(self._name)


# --- build_bot ---------------------------------------------------------------


async def test_build_bot_uses_token_and_api_base() -> None:
    config = _settings(api={"base": "http://127.0.0.1:8081"})
    bot = build_bot(config)
    try:
        assert bot.token == "123456:TESTTOKEN"
        assert bot.session.api.base.startswith("http://127.0.0.1:8081")
        assert bot.default.parse_mode is ParseMode.HTML
    finally:
        await bot.session.close()


# --- build_app ---------------------------------------------------------------


async def test_build_app_wires_services(monkeypatch: pytest.MonkeyPatch) -> None:
    config = _settings(sudo_ids=[7])
    bot = build_bot(config)

    async def fake_get_me() -> Any:
        return type("Me", (), {"id": 999, "username": "mock_bot"})()

    monkeypatch.setattr(bot, "get_me", fake_get_me)
    try:
        app = await build_app(config, bot)
        try:
            assert isinstance(app, App)
            assert app.bot is bot
            assert app.bot_id == 999
            assert app.bot_username == "mock_bot"
            assert app.sudo_ids == {7}
            assert isinstance(app.dp, Dispatcher)
            assert app.metrics is not None
        finally:
            await app.db.close()
    finally:
        await bot.session.close()


# --- _run_polling ------------------------------------------------------------


def _fake_app(dp: Any) -> Any:
    """A minimal App stand-in carrying just what the update sources read."""
    return SimpleNamespace(dp=dp, bot=object(), config=_settings())


class _PollingDispatcher:
    """A dispatcher double whose ``start_polling`` returns promptly."""

    def __init__(self) -> None:
        self.started = False
        self.stopped = False

    async def start_polling(self, *args: Any, **kwargs: Any) -> None:
        self.started = True
        await asyncio.sleep(0)

    async def stop_polling(self) -> None:
        self.stopped = True


async def test_run_polling_starts_then_drains() -> None:
    dp = _PollingDispatcher()
    stop = asyncio.Event()
    stop.set()
    await _run_polling(_fake_app(dp), stop)
    assert dp.started is True
    assert dp.stopped is True


async def test_run_polling_cancels_a_hung_drain(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(main_module, "SHUTDOWN_DRAIN_SECONDS", 0.01)

    class _HungDispatcher:
        async def start_polling(self, *args: Any, **kwargs: Any) -> None:
            await asyncio.Event().wait()  # never returns

        async def stop_polling(self) -> None:
            return None

    stop = asyncio.Event()
    stop.set()
    # The drain must time out and cancel the polling task rather than hang.
    await _run_polling(_fake_app(_HungDispatcher()), stop)


# --- _run_webhook ------------------------------------------------------------


async def test_run_webhook_setup_and_cleanup(monkeypatch: pytest.MonkeyPatch) -> None:
    events: list[str] = []

    class _Runner:
        def __init__(self, web_app: Any) -> None:
            self.web_app = web_app

        async def setup(self) -> None:
            events.append("setup")

        async def cleanup(self) -> None:
            events.append("cleanup")

    class _Site:
        def __init__(self, runner: Any, host: str, port: int) -> None:
            events.append(f"site:{host}:{port}")

        async def start(self) -> None:
            events.append("start")

    def fake_build_webhook_app(config: Any, dp: Any, bot: Any) -> Any:
        return object()

    monkeypatch.setattr(main_module, "build_webhook_app", fake_build_webhook_app)
    monkeypatch.setattr(web, "AppRunner", _Runner)
    monkeypatch.setattr(web, "TCPSite", _Site)

    stop = asyncio.Event()
    stop.set()
    await _run_webhook(_fake_app(object()), stop)
    assert events == ["setup", "site:127.0.0.1:8080", "start", "cleanup"]


# --- signal handlers and cache clearing --------------------------------------


def test_install_signal_handlers_registers_int_and_term(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    recorded: list[Any] = []

    class _Loop:
        def add_signal_handler(self, sig: Any, callback: Any, *args: Any) -> None:
            recorded.append(sig)

    monkeypatch.setattr(asyncio, "get_running_loop", _Loop)
    _install_signal_handlers(asyncio.Event())
    assert recorded == [signal.SIGINT, signal.SIGTERM]


def test_install_metrics_dump_registers_usr1(monkeypatch: pytest.MonkeyPatch) -> None:
    recorded: list[Any] = []

    class _Loop:
        def add_signal_handler(self, sig: Any, callback: Any, *args: Any) -> None:
            recorded.append((sig, callback))

    monkeypatch.setattr(asyncio, "get_running_loop", _Loop)
    metrics = Metrics()
    _install_metrics_dump(metrics)
    assert recorded == [(signal.SIGUSR1, metrics.dump)]


def test_clear_aiogram_caches_empties_the_update_cache() -> None:
    _clear_aiogram_caches()
    descriptor = Update.__dict__["event_type"]
    assert descriptor.fget.cache_info().currsize == 0


# --- run_bot -----------------------------------------------------------------


def _make_run_app(config: Settings, log: list[str], metrics: Metrics | None) -> Any:
    """A minimal App stand-in for the ``run_bot`` shutdown path."""
    return SimpleNamespace(
        config=config,
        bot=SimpleNamespace(session=_Closer("bot.session", log)),
        dp=object(),
        db=_Closer("db", log),
        expiry=_Closer("expiry", log),
        captcha_sweeper=_Closer("captcha_sweeper", log),
        metrics=metrics,
        clock=FakeClock(),
        bot_id=1,
        bot_username="b",
    )


def _wire_run_bot(
    monkeypatch: pytest.MonkeyPatch, app: Any, log: list[str], calls: list[str]
) -> None:
    """Replace every heavy collaborator ``run_bot`` reaches for."""

    def fake_build_bot(cfg: Settings) -> Any:
        return SimpleNamespace(session=_Closer("bot.session", log))

    def fake_install_rate_limiter(bot: Any, limiter: Any) -> None:
        calls.append("rate_limiter")

    def fake_install_signal_handlers(stop: Any) -> None:
        calls.append("signals")

    def fake_install_metrics_dump(metrics: Any) -> None:
        calls.append("metrics_dump")

    async def fake_build_app(cfg: Settings, bot: Any) -> Any:
        return app

    async def fake_publish(bot: Any) -> None:
        calls.append("publish")

    async def fake_run_polling(a: Any, s: Any) -> None:
        calls.append("polling")

    async def fake_run_webhook(a: Any, s: Any) -> None:
        calls.append("webhook")

    monkeypatch.setattr(main_module, "build_bot", fake_build_bot)
    monkeypatch.setattr(main_module, "install_rate_limiter", fake_install_rate_limiter)
    monkeypatch.setattr(main_module, "build_app", fake_build_app)
    monkeypatch.setattr(main_module, "publish_commands", fake_publish)
    monkeypatch.setattr(main_module, "_install_signal_handlers", fake_install_signal_handlers)
    monkeypatch.setattr(main_module, "_install_metrics_dump", fake_install_metrics_dump)
    monkeypatch.setattr(main_module, "_run_polling", fake_run_polling)
    monkeypatch.setattr(main_module, "_run_webhook", fake_run_webhook)


@pytest.mark.parametrize("mode", [Mode.POLLING, Mode.WEBHOOK])
async def test_run_bot_starts_the_configured_source_and_closes(
    monkeypatch: pytest.MonkeyPatch, mode: Mode
) -> None:
    config = _settings(mode=mode.value, webhook={"secret": "0123456789abcdef"})
    log: list[str] = []
    calls: list[str] = []
    app = _make_run_app(config, log, Metrics())
    _wire_run_bot(monkeypatch, app, log, calls)

    assert await run_bot(config) == 0

    source = "webhook" if mode is Mode.WEBHOOK else "polling"
    assert calls == ["rate_limiter", "publish", "signals", "metrics_dump", source]
    for name in ("expiry", "captcha_sweeper", "db", "bot.session"):
        assert name in log


async def test_run_bot_without_metrics_skips_the_monitor(monkeypatch: pytest.MonkeyPatch) -> None:
    config = _settings()
    log: list[str] = []
    calls: list[str] = []
    app = _make_run_app(config, log, None)
    _wire_run_bot(monkeypatch, app, log, calls)

    assert await run_bot(config) == 0
    assert "metrics_dump" not in calls
    assert "polling" in calls
    assert "db" in log


# --- main --------------------------------------------------------------------


def test_main_version_flag(capsys: pytest.CaptureFixture[str]) -> None:
    assert main(["--version"]) == 0
    assert "nacho-moderator" in capsys.readouterr().out


def test_main_check_config_ok(capsys: pytest.CaptureFixture[str], tmp_path: Path) -> None:
    assert main(["--check-config", str(_secure_config(tmp_path))]) == 0
    assert "ok" in capsys.readouterr().out


def test_main_config_error_returns_one(capsys: pytest.CaptureFixture[str], tmp_path: Path) -> None:
    assert main(["--config", str(tmp_path / "missing.toml")]) == 1
    assert "error" in capsys.readouterr().err


def test_main_runs_the_bot(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    seen: list[Settings] = []

    async def fake_run_bot(config: Settings) -> int:
        seen.append(config)
        return 0

    def fake_asyncio_run(coro: Any) -> int:
        loop = asyncio.new_event_loop()
        try:
            return loop.run_until_complete(coro)
        finally:
            loop.close()

    monkeypatch.setattr(main_module, "run_bot", fake_run_bot)
    monkeypatch.setattr(asyncio, "run", fake_asyncio_run)

    assert main(["--config", str(_secure_config(tmp_path))]) == 0
    assert len(seen) == 1
    assert seen[0].bot_token == "123456:TESTTOKEN"


def _fake_asyncio_run_raising(exc: BaseException) -> Any:
    """Build an ``asyncio.run`` double that closes the coroutine and raises."""

    def _fake(coro: Any) -> int:
        coro.close()
        raise exc

    return _fake


def test_main_keyboard_interrupt_is_a_clean_exit(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    monkeypatch.setattr(asyncio, "run", _fake_asyncio_run_raising(KeyboardInterrupt()))
    assert main(["--config", str(_secure_config(tmp_path))]) == 0


def test_main_runtime_error_returns_one(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    monkeypatch.setattr(asyncio, "run", _fake_asyncio_run_raising(RuntimeError("boom")))
    assert main(["--config", str(_secure_config(tmp_path))]) == 1
