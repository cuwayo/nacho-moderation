#!/usr/bin/env python3
"""test_integration_getme.py - drive the real loop against the mock server (A5.3).

Starts tests/mock/mock_tg_server.py, points the built `tgbot` at it over plain
HTTP via the environment (no TLS, no real token), and asserts the getMe probe
succeeds. The default run then polls forever, so the process is stopped with
SIGTERM and must exit 0. Skipped when the binary is unavailable.

Run: python3 tests/integration/test_integration_getme.py
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


@unittest.skipIf(BINARY is None, "no tgbot binary built")
class GetMeIntegrationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.port = free_port()
        self.tmpdir = tempfile.TemporaryDirectory(prefix="nm_it_")
        self.addCleanup(self.tmpdir.cleanup)
        self.log = Path(self.tmpdir.name) / "calls.jsonl"
        self.server = subprocess.Popen(
            [sys.executable, str(REPO_ROOT / "tests" / "mock" / "mock_tg_server.py"),
             "--port", str(self.port), "--log", str(self.log)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.addCleanup(self.server.terminate)
        self._wait_for_server()

    def _wait_for_server(self) -> None:
        for _ in range(50):
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                    return
            except OSError:
                time.sleep(0.1)
        self.fail("mock server did not start")

    def _run_until_getme(self) -> subprocess.Popen:
        env = dict(os.environ)
        env["BOT_TOKEN"] = "123456:TEST"
        env["API_BASE"] = f"http://127.0.0.1:{self.port}"
        proc = subprocess.Popen(
            [str(BINARY)],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        return proc

    def test_getme_against_mock_server(self) -> None:
        proc = self._run_until_getme()
        self.addCleanup(proc.kill)

        # Wait for the getMe probe line, then stop the (now polling) bot.
        assert proc.stdout is not None
        deadline = time.time() + 20
        line = ""
        while time.time() < deadline:
            line = proc.stdout.readline()
            if not line:
                break
            if "getMe ok" in line:
                break
        self.assertIn("getMe ok", line, msg=f"stdout so far: {line!r}")

        proc.terminate()
        try:
            rc = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            self.fail("tgbot did not exit after SIGTERM")
        self.assertEqual(rc, 0)

        calls = [json.loads(x) for x in self.log.read_text().splitlines()]
        self.assertTrue(any(c["method"] == "getMe" for c in calls))


if __name__ == "__main__":
    unittest.main()
