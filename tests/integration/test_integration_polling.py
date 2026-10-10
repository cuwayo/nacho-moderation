#!/usr/bin/env python3
"""test_integration_polling.py - getUpdates long-polling against the mock server.

Drives the real `tgbot` against tests/mock/mock_tg_server.py over plain HTTP and
asserts:
  * getUpdates is called with an explicit `allowed_updates` list;
  * the offset advances past the received update id (0 -> 11 after update_id 10);
  * a 429 on the first getUpdates is honoured (the same offset is retried).

The default run polls until SIGTERM, so each test starts the process, waits for
the expected calls, then stops it. Skipped when the binary is unavailable.

Run: python3 tests/integration/test_integration_polling.py
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
SCENARIOS = REPO_ROOT / "tests" / "mock" / "scenarios"


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
class PollingIntegrationTest(unittest.TestCase):
    def _start(self, scenario: str | None) -> tuple[subprocess.Popen, Path]:
        self.port = free_port()
        self.tmpdir = tempfile.TemporaryDirectory(prefix="nm_poll_")
        self.addCleanup(self.tmpdir.cleanup)
        self.log = Path(self.tmpdir.name) / "calls.jsonl"
        cmd = [sys.executable, str(REPO_ROOT / "tests" / "mock" / "mock_tg_server.py"),
               "--port", str(self.port), "--log", str(self.log)]
        if scenario is not None:
            cmd += ["--scenario", str(SCENARIOS / scenario)]
        self.server = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(self.server.terminate)
        wait_for_server(self.port)

        env = dict(os.environ)
        env["BOT_TOKEN"] = "123456:TEST"
        env["API_BASE"] = f"http://127.0.0.1:{self.port}"
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

    def test_offset_advances_and_allowed_updates_sent(self) -> None:
        proc, log = self._start("polling_429.json")
        # Wait until a getUpdates with offset 11 appears (update_id 10 consumed).
        ok = self._wait_for(
            log,
            lambda calls: any(
                c["method"] == "getUpdates" and (c.get("json") or {}).get("offset") == 11
                for c in calls
            ),
        )
        self._stop(proc)
        self.assertTrue(ok, msg=f"calls: {read_calls(log)}")

        calls = read_calls(log)
        updates = [c for c in calls if c["method"] == "getUpdates"]
        self.assertTrue(updates, "no getUpdates calls recorded")
        first = updates[0]["json"]
        self.assertEqual(
            first["allowed_updates"],
            ["message", "edited_message", "callback_query", "chat_member", "my_chat_member"],
        )
        self.assertEqual(first["timeout"], 30)
        # The 429 fault forced a retry of the same offset (0) before advancing.
        offsets = [u["json"]["offset"] for u in updates]
        self.assertIn(0, offsets)
        self.assertIn(11, offsets)

    def test_429_is_retried_same_offset(self) -> None:
        proc, log = self._start("polling_429.json")
        # The 429 arrives on the first getUpdates; the retry uses the same offset 0.
        ok = self._wait_for(
            log,
            lambda calls: sum(
                1 for c in calls if c["method"] == "getUpdates"
                and (c.get("json") or {}).get("offset") == 0
            )
            >= 2,
        )
        self._stop(proc)
        self.assertTrue(ok, msg=f"429 not retried; calls: {read_calls(log)}")


if __name__ == "__main__":
    unittest.main()
