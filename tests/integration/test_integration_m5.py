#!/usr/bin/env python3
"""test_integration_m5.py - M5 warns/settings/welcome end to end.

Drives the real `tgbot` against tests/mock/mock_tg_server.py over plain HTTP and
asserts the M5 paths (SPEC-ADDENDUM-TOOLCHAIN A5.3):
  * three `/warn` commands reach the default limit (3) and trigger the configured
    `mute` action via `restrictChatMember`;
  * `/setwelcome` + `/welcome on` then a join delivers the rendered welcome via
    `getChatMemberCount` + `sendMessage`;
  * `/settings` sends an inline keyboard, and a button press from the owner is
    answered with `answerCallbackQuery` and re-rendered with `editMessageText`.

The default run polls until SIGTERM, so each test starts the process, waits for
the expected calls, then stops it. Skipped when the binary is unavailable.

Run: python3 tests/integration/test_integration_m5.py
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
class M5IntegrationTest(unittest.TestCase):
    def _start(self, scenario: str, db_path: Path | None = None) -> tuple[subprocess.Popen, Path]:
        self.port = free_port()
        self.tmpdir = tempfile.TemporaryDirectory(prefix="nm_m5_")
        self.addCleanup(self.tmpdir.cleanup)
        self.log = Path(self.tmpdir.name) / "calls.jsonl"
        self.db = db_path if db_path is not None else Path(self.tmpdir.name) / "bot.db"
        cmd = [sys.executable, str(REPO_ROOT / "tests" / "mock" / "mock_tg_server.py"),
               "--port", str(self.port), "--log", str(self.log),
               "--scenario", str(SCENARIOS / scenario)]
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

    def _wait_for(self, log: Path, predicate, timeout: float = 25.0) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if predicate(read_calls(log)):
                return True
            time.sleep(0.1)
        return False

    def test_warn_limit_triggers_configured_action(self) -> None:
        proc, log = self._start("warn_limit_action.json")
        ok = self._wait_for(
            log, lambda calls: any(c["method"] == "restrictChatMember" for c in calls)
        )
        self._stop(proc)
        self.assertTrue(ok, msg=f"no restrictChatMember; calls: {read_calls(log)}")

        calls = read_calls(log)
        # Three warns were recorded before the limit action fired.
        mute = next(c for c in calls if c["method"] == "restrictChatMember")
        self.assertEqual(mute["json"]["user_id"], 300)
        self.assertEqual(mute["json"]["chat_id"], -1001234567890)
        self.assertFalse(mute["json"]["permissions"]["can_send_messages"])
        # The bot told the admin the limit was reached.
        texts = [c["json"]["text"] for c in calls
                 if c["method"] == "sendMessage" and c.get("json")]
        self.assertTrue(any("limit" in t.lower() for t in texts), msg=f"texts: {texts}")

    def test_join_delivers_rendered_welcome(self) -> None:
        # Part 1: configure the welcome and stop, so the DB is committed.
        shared_db = Path(tempfile.mkdtemp(prefix="nm_m5_db_")) / "bot.db"
        self.addCleanup(lambda: __import__("shutil").rmtree(shared_db.parent, ignore_errors=True))
        proc1, log1 = self._start("welcome_join.json", db_path=shared_db)
        ok1 = self._wait_for(
            log1,
            lambda calls: any(
                c["method"] == "sendMessage" and c.get("json")
                and "Welcome enabled" in c["json"].get("text", "")
                for c in calls
            ),
        )
        self._stop(proc1)
        self.assertTrue(ok1, msg=f"welcome not enabled; calls: {read_calls(log1)}")

        # Part 2: with the welcome persisted, a join must be greeted.
        proc2, log2 = self._start("welcome_join_part2.json", db_path=shared_db)
        ok2 = self._wait_for(
            log2, lambda calls: any(c["method"] == "getChatMemberCount" for c in calls)
        )
        self._stop(proc2)
        self.assertTrue(ok2, msg=f"no getChatMemberCount; calls: {read_calls(log2)}")

        calls = read_calls(log2)
        welcome = [c for c in calls
                   if c["method"] == "sendMessage"
                   and c.get("json") and "Welcome" in c["json"].get("text", "")]
        self.assertTrue(welcome, msg=f"no rendered welcome; calls: {calls}")
        text = welcome[-1]["json"]["text"]
        # {first} and {chat} were substituted from the join and the chat title.
        self.assertIn("Newbie", text)
        self.assertIn("Testers", text)
        self.assertNotIn("{first}", text)

    def test_settings_panel_toggle_edits_in_place(self) -> None:
        proc, log = self._start("settings_panel.json")
        ok = self._wait_for(
            log, lambda calls: any(c["method"] == "editMessageText" for c in calls)
        )
        self._stop(proc)
        self.assertTrue(ok, msg=f"no editMessageText; calls: {read_calls(log)}")

        calls = read_calls(log)
        # The panel was first sent with an inline keyboard.
        panels = [c for c in calls
                  if c["method"] == "sendMessage"
                  and c.get("json") and "reply_markup" in c["json"]]
        self.assertTrue(panels, msg=f"no inline keyboard panel; calls: {calls}")
        # The press was acknowledged and the panel edited in place.
        self.assertTrue(any(c["method"] == "answerCallbackQuery" for c in calls))
        edit = next(c for c in calls if c["method"] == "editMessageText")
        self.assertEqual(edit["json"]["chat_id"], -1001234567890)
        self.assertEqual(edit["json"]["message_id"], 10)
        self.assertIn("reply_markup", edit["json"])


if __name__ == "__main__":
    unittest.main()
