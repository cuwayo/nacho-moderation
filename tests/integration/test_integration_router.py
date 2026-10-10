#!/usr/bin/env python3
"""test_integration_router.py - update routing against the mock Telegram server.

Drives the real `tgbot` against tests/mock/mock_tg_server.py over plain HTTP and
asserts the M3 path end to end:
  * a scripted `/help` message is routed: the caller's role is resolved via
    `getChatAdministrators`, and the bot replies with `sendMessage`;
  * the bot's command menu is registered once at startup via `setMyCommands`;
  * a non-command message triggers no reply.

The default run polls until SIGTERM, so each test starts the process, waits for
the expected calls, then stops it. Skipped when the binary is unavailable.

Run: python3 tests/integration/test_integration_router.py
Env: NM_TGBOT_BIN overrides the binary path.
"""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def find_binary() -> Path | None:
    env = os.environ.get("NM_TGBOT_BIN")
    candidates = [Path(env)] if env else []
    candidates += [REPO_ROOT / "build" / p / "tgbot" for p in ("debug", "asan", "release")]
    for c in candidates:
        if c.is_file():
            return c
    return None


BINARY = find_binary()


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for_server(port: int) -> None:
    for _ in range(50):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("mock server did not start")


def read_calls(log: Path) -> list[dict]:
    if not log.is_file():
        return []
    return [json.loads(x) for x in log.read_text().splitlines()]


@unittest.skipIf(BINARY is None, "no tgbot binary built")
class RouterIntegrationTest(unittest.TestCase):
    def _start(self, scenario: str | None) -> tuple[subprocess.Popen, Path]:
        self.port = free_port()
        self.tmpdir = tempfile.TemporaryDirectory(prefix="nm_router_")
        self.addCleanup(self.tmpdir.cleanup)
        self.log = Path(self.tmpdir.name) / "calls.jsonl"
        self.db = Path(self.tmpdir.name) / "bot.db"
        cmd = [sys.executable, str(REPO_ROOT / "tests" / "mock" / "mock_tg_server.py"),
               "--port", str(self.port), "--log", str(self.log)]
        if scenario is not None:
            cmd += ["--scenario", str(REPO_ROOT / "tests" / "mock" / "scenarios" / scenario)]
        self.server = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(self.server.terminate)
        wait_for_server(self.port)

        env = dict(os.environ)
        env["BOT_TOKEN"] = "123456:TEST"
        env["API_BASE"] = f"http://127.0.0.1:{self.port}"
        env["DB_PATH"] = str(self.db)
        env["SUDO_IDS"] = ""
        proc = subprocess.Popen([str(BINARY)], env=env, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        self.addCleanup(proc.kill)
        return proc, self.log

    def _stop(self, proc: subprocess.Popen) -> None:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    def _wait_for(self, log: Path, predicate, timeout: float = 20.0) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if predicate(read_calls(log)):
                return True
            time.sleep(0.1)
        return False

    def test_help_message_gets_a_reply(self) -> None:
        proc, log = self._start("router_help.json")
        ok = self._wait_for(
            log,
            lambda calls: any(c["method"] == "sendMessage" for c in calls),
        )
        self._stop(proc)
        self.assertTrue(ok, msg=f"no sendMessage; calls: {read_calls(log)}")

        calls = read_calls(log)
        # The caller's role was resolved before dispatch.
        self.assertTrue(any(c["method"] == "getChatAdministrators" for c in calls))
        reply = next(c for c in calls if c["method"] == "sendMessage")
        self.assertIn("/help", reply["json"]["text"])
        self.assertEqual(reply["json"]["parse_mode"], "HTML")

    def test_private_chat_help_replies_without_admin_fetch(self) -> None:
        """Regression: /help in a private chat must not call getChatAdministrators.

        Telegram rejects that method for private chats (ok=false -> ERR_TG_API),
        which previously made the router drop the command. The scenario injects
        that rejection as a tripwire; the router must skip the call entirely and
        still reply.
        """
        proc, log = self._start("router_private_help.json")
        ok = self._wait_for(log, lambda calls: any(c["method"] == "sendMessage" for c in calls))
        self._stop(proc)
        self.assertTrue(ok, msg=f"no sendMessage; calls: {read_calls(log)}")

        calls = read_calls(log)
        self.assertFalse(
            any(c["method"] == "getChatAdministrators" for c in calls),
            msg=f"private chat must not fetch admins; calls: {calls}",
        )
        reply = next(c for c in calls if c["method"] == "sendMessage")
        self.assertIn("/help", reply["json"]["text"])

    def test_set_my_commands_registered_at_startup(self) -> None:
        proc, log = self._start("router_help.json")
        ok = self._wait_for(log, lambda calls: any(c["method"] == "setMyCommands" for c in calls))
        self._stop(proc)
        self.assertTrue(ok, msg=f"no setMyCommands; calls: {read_calls(log)}")

    def test_non_command_message_is_ignored(self) -> None:
        proc, log = self._start("router_plain.json")
        # Give the bot time to poll the plain message, then assert no reply.
        self._wait_for(
            log,
            lambda calls: any(
                c["method"] == "getUpdates" and (c.get("json") or {}).get("offset", 0) > 0
                for c in calls
            ),
        )
        time.sleep(0.5)
        self._stop(proc)
        calls = read_calls(log)
        self.assertFalse(any(c["method"] == "sendMessage" for c in calls), msg=f"calls: {calls}")


if __name__ == "__main__":
    unittest.main()
