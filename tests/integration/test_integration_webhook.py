#!/usr/bin/env python3
"""test_integration_webhook.py - webhook mode end to end (SPEC-ADDENDUM-TOOLCHAIN A5.3).

Runs the real `tgbot` with `MODE=webhook`, pointed at the mock Bot API for the
getMe probe, and posts updates to its own plain-HTTP listen socket. Asserts:
  * a POST with the correct secret token is accepted (200) and routed as an
    update (the command produces an outbound `sendMessage` on the mock server);
  * a POST with a wrong secret is rejected (403) and produces no outbound call;
  * a GET is rejected (405);
  * SIGTERM shuts the process down with exit code 0 within 10 s.

Skipped when the binary is unavailable.

Run: python3 tests/integration/test_integration_webhook.py
Env: NM_TGBOT_BIN overrides the binary path.
"""

from __future__ import annotations

import http.client
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


def wait_for_port(port: int, timeout: float = 10.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def read_calls(log: Path) -> list[dict]:
    if not log.is_file():
        return []
    return [json.loads(x) for x in log.read_text().splitlines()]


# A single Update carrying a /help command in a supergroup.
HELP_UPDATE = {
    "update_id": 1,
    "message": {
        "message_id": 5,
        "date": 1700000000,
        "chat": {"id": -1001234567890, "type": "supergroup", "title": "Test"},
        "from": {"id": 200, "is_bot": False, "first_name": "Admin", "username": "admin"},
        "text": "/help",
        "entities": [{"type": "bot_command", "offset": 0, "length": 5}],
    },
}


@unittest.skipIf(BINARY is None, "no tgbot binary built")
class WebhookIntegrationTest(unittest.TestCase):
    def _start(self) -> tuple[subprocess.Popen, Path, int]:
        api_port = free_port()
        webhook_port = free_port()
        self.tmpdir = tempfile.TemporaryDirectory(prefix="nm_wh_")
        self.addCleanup(self.tmpdir.cleanup)
        self.log = Path(self.tmpdir.name) / "calls.jsonl"

        server = subprocess.Popen(
            [sys.executable, str(REPO_ROOT / "tests" / "mock" / "mock_tg_server.py"),
             "--port", str(api_port), "--log", str(self.log)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(server.terminate)
        assert wait_for_port(api_port), "mock server did not start"

        env = dict(os.environ)
        env["BOT_TOKEN"] = "123456:TEST"
        env["API_BASE"] = f"http://127.0.0.1:{api_port}"
        env["MODE"] = "webhook"
        env["WEBHOOK_LISTEN"] = f"127.0.0.1:{webhook_port}"
        env["WEBHOOK_SECRET"] = "s3cr3t"
        env["DB_PATH"] = str(Path(self.tmpdir.name) / "bot.db")
        env["SUDO_IDS"] = ""
        proc = subprocess.Popen([str(BINARY)], env=env, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        self.addCleanup(proc.kill)
        self.assertTrue(wait_for_port(webhook_port), "webhook server did not start")
        return proc, self.log, webhook_port

    def _post(self, port: int, body: dict, secret: str | None) -> tuple[int, str]:
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        headers = {"Content-Type": "application/json"}
        if secret is not None:
            headers["X-Telegram-Bot-Api-Secret-Token"] = secret
        conn.request("POST", "/webhook", json.dumps(body), headers)
        resp = conn.getresponse()
        text = resp.read().decode("utf-8", "replace")
        conn.close()
        return resp.status, text

    def _wait_for_call(self, log: Path, predicate, timeout: float = 15.0) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if predicate(read_calls(log)):
                return True
            time.sleep(0.1)
        return False

    def test_valid_update_is_routed(self) -> None:
        proc, log, port = self._start()
        status, _ = self._post(port, HELP_UPDATE, "s3cr3t")
        self.assertEqual(status, 200)
        # /help from an admin produces a sendMessage via the mock Bot API.
        ok = self._wait_for_call(log, lambda calls: any(c["method"] == "sendMessage" for c in calls))
        proc.terminate()
        self.assertEqual(proc.wait(timeout=10), 0)
        self.assertTrue(ok, msg=f"no sendMessage; calls: {read_calls(log)}")

    def test_wrong_secret_is_rejected(self) -> None:
        proc, log, port = self._start()
        status, _ = self._post(port, HELP_UPDATE, "wrong")
        self.assertEqual(status, 403)
        # No outbound call should follow from a rejected update.
        time.sleep(1.0)
        proc.terminate()
        proc.wait(timeout=10)
        sends = [c for c in read_calls(log) if c["method"] == "sendMessage"]
        self.assertEqual(sends, [], msg=f"unexpected sends: {sends}")

    def test_get_is_rejected(self) -> None:
        proc, _log, port = self._start()
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        conn.request("GET", "/webhook")
        resp = conn.getresponse()
        resp.read()
        conn.close()
        proc.terminate()
        proc.wait(timeout=10)
        self.assertEqual(resp.status, 405)

    def test_sigterm_drains_and_exits_zero(self) -> None:
        proc, _log, _port = self._start()
        proc.terminate()
        try:
            rc = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            self.fail("tgbot did not exit after SIGTERM")
        self.assertEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()
