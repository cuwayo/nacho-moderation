"""Integration harness: drive the real bot against the mock Bot API server.

Unlike the unit tests, these start the actual ``nacho_moderator`` process with
``API_BASE`` pointed at ``tests/mock/mock_tg_server.py`` and assert on the calls
the mock recorded. They are skipped automatically if the mock server cannot
start; they never touch the network.
"""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import time
from collections.abc import Callable, Iterator
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
MOCK_SERVER = REPO_ROOT / "tests" / "mock" / "mock_tg_server.py"
SCENARIOS = REPO_ROOT / "tests" / "mock" / "scenarios"

# Generous bounds; a command is processed in well under a second once the bot is up.
CALL_TIMEOUT_SECONDS = 20.0
STARTUP_TIMEOUT_SECONDS = 30.0


def _free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _wait_for_port(port: int, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"mock server did not open port {port}")


def _read_calls(log: Path) -> list[dict]:
    if not log.is_file():
        return []
    out: list[dict] = []
    for line in log.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line:
            out.append(json.loads(line))
    return out


class IntegrationRun:
    """A running bot + mock server pair, with helpers to await recorded calls."""

    def __init__(self, calls_log: Path, bot_log: Path, bot: subprocess.Popen[bytes]) -> None:
        self.calls_log = calls_log
        self.bot_log = bot_log
        self.bot = bot

    def calls(self) -> list[dict]:
        """Return every recorded API call."""
        return _read_calls(self.calls_log)

    def methods(self) -> list[str]:
        """Return the recorded API method names, in order."""
        return [c.get("method", "") for c in self.calls()]

    def wait_for(self, method: str, *, timeout: float = CALL_TIMEOUT_SECONDS) -> dict:
        """Block until a call to ``method`` is recorded, and return it.

        Raises:
            AssertionError: If the call never arrives within ``timeout``.
        """
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for call in self.calls():
                if call.get("method") == method:
                    return call
            if self.bot.poll() is not None:
                break
            time.sleep(0.1)
        raise AssertionError(
            f"{method} was never called; recorded={self.methods()}\nbot log:\n{self.bot_output()}"
        )

    def bot_output(self) -> str:
        """Return the bot's combined stdout/stderr."""
        return self.bot_log.read_text(encoding="utf-8", errors="replace")


@pytest.fixture
def start_bot(tmp_path: Path) -> Iterator[Callable[..., IntegrationRun]]:
    """Factory fixture: start the bot against a named scenario."""
    cleanups: list[Callable[[], None]] = []

    def _start(scenario: str, *, extra_env: dict[str, str] | None = None) -> IntegrationRun:
        port = _free_port()
        calls_log = tmp_path / f"calls-{port}.jsonl"
        server_log = tmp_path / f"server-{port}.log"
        bot_log = tmp_path / f"bot-{port}.log"
        db_path = tmp_path / f"bot-{port}.db"
        config_path = tmp_path / f"config-{port}.toml"
        config_path.write_text(
            'bot_token = "123456:TEST"\nmode = "polling"\n'
            f'[api]\nbase = "http://127.0.0.1:{port}"\n'
            f'[db]\npath = "{db_path}"\n'
            '[polling]\ntimeout = 1\n[log]\nlevel = "info"\n',
            encoding="utf-8",
        )
        config_path.chmod(0o600)

        # Keep the log file handles open for the child's lifetime; they are closed
        # in the cleanup below (closing them while the process runs would raise).
        server_handle = server_log.open("wb")
        bot_handle = bot_log.open("wb")
        handles = (server_handle, bot_handle)

        server = subprocess.Popen(
            [
                sys.executable,
                str(MOCK_SERVER),
                "--port",
                str(port),
                "--log",
                str(calls_log),
                "--scenario",
                str(SCENARIOS / scenario),
            ],
            stdout=server_handle,
            stderr=subprocess.STDOUT,
        )
        try:
            _wait_for_port(port, STARTUP_TIMEOUT_SECONDS)
        except RuntimeError:
            server.kill()
            server.wait(timeout=10)
            for handle in handles:
                handle.close()
            pytest.skip("mock Bot API server did not start")

        env = dict(os.environ)
        # Keep the environment hermetic: drop any inherited config the test host
        # might have, and point the bot at the scenario config file.
        for key in ("MODE", "API_BASE", "BOT_TOKEN", "DB_PATH", "LOG_LEVEL", "POLLING_TIMEOUT"):
            env.pop(key, None)
        if extra_env:
            env.update(extra_env)

        bot = subprocess.Popen(
            [sys.executable, "-m", "nacho_moderator", "--config", str(config_path)],
            cwd=str(REPO_ROOT),
            env=env,
            stdout=bot_handle,
            stderr=subprocess.STDOUT,
        )

        def _cleanup() -> None:
            for proc in (bot, server):
                if proc.poll() is None:
                    proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=10)
            for handle in handles:
                handle.close()

        cleanups.append(_cleanup)
        return IntegrationRun(calls_log, bot_log, bot)

    yield _start

    for cleanup in reversed(cleanups):
        cleanup()
