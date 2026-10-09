#!/usr/bin/env python3
"""test_integration_getme.py - drive the real loop against the mock server (A5.3).

Starts tests/mock/mock_tg_server.py, points the built `tgbot` at it over plain
HTTP via the environment (no TLS, no real token), and asserts a successful getMe.
Skipped when the binary is unavailable.

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

    def test_getme_against_mock_server(self) -> None:
        env = dict(os.environ)
        env["BOT_TOKEN"] = "123456:TEST"
        env["API_BASE"] = f"http://127.0.0.1:{self.port}"
        proc = subprocess.run(
            [str(BINARY)],
            env=env,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(proc.returncode, 0, msg=proc.stderr)
        self.assertIn("getMe ok", proc.stdout)
        self.assertIn("mock_bot", proc.stdout)

        calls = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertTrue(any(c["method"] == "getMe" for c in calls))


if __name__ == "__main__":
    unittest.main()
