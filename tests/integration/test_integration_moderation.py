#!/usr/bin/env python3
"""test_integration_moderation.py - M4 moderation commands end to end.

Drives the real `tgbot` against tests/mock/mock_tg_server.py over plain HTTP and
asserts the M4 path (SPEC-ADDENDUM-TOOLCHAIN A5.3 scenarios 1-4):
  * an admin `/ban 10m` on a reply issues `banChatMember` for the replied-to user
    with an `until_date` roughly ten minutes ahead, and records the action;
  * a non-admin `/ban` sends no `banChatMember` (an error reply instead);
  * `/kick` issues a `banChatMember` followed by an `unbanChatMember`.

Skipped when the binary is unavailable.

Run: python3 tests/integration/test_integration_moderation.py
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
class ModerationIntegrationTest(unittest.TestCase):
    def _start(self, scenario: str, env_extra: dict | None = None) -> tuple[subprocess.Popen, Path]:
        self.port = free_port()
        self.tmpdir = tempfile.TemporaryDirectory(prefix="nm_mod_")
        self.addCleanup(self.tmpdir.cleanup)
        self.log = Path(self.tmpdir.name) / "calls.jsonl"
        self.db = Path(self.tmpdir.name) / "bot.db"
        cmd = [sys.executable, str(REPO_ROOT / "tests" / "mock" / "mock_tg_server.py"),
               "--port", str(self.port), "--log", str(self.log),
               "--scenario", str(REPO_ROOT / "tests" / "mock" / "scenarios" / scenario)]
        self.server = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(self.server.terminate)
        wait_for_server(self.port)

        env = dict(os.environ)
        env["BOT_TOKEN"] = "123456:TEST"
        env["API_BASE"] = f"http://127.0.0.1:{self.port}"
        env["DB_PATH"] = str(self.db)
        env["SUDO_IDS"] = ""
        if env_extra:
            env.update(env_extra)
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

    def test_admin_ban_reply_issues_ban_with_until_date(self) -> None:
        proc, log = self._start("ban_reply.json")
        ok = self._wait_for(log, lambda calls: any(c["method"] == "banChatMember" for c in calls))
        self._stop(proc)
        self.assertTrue(ok, msg=f"no banChatMember; calls: {read_calls(log)}")

        calls = read_calls(log)
        ban = next(c for c in calls if c["method"] == "banChatMember")
        self.assertEqual(ban["json"]["user_id"], 300)  # the replied-to user
        self.assertEqual(ban["json"]["chat_id"], -1001234567890)
        # until_date is ~10 minutes ahead of the current wall clock.
        now = time.time()
        self.assertGreaterEqual(ban["json"]["until_date"], now + 600 - 120)
        self.assertLessEqual(ban["json"]["until_date"], now + 600 + 120)

    def test_non_admin_ban_makes_no_api_call(self) -> None:
        proc, log = self._start("ban_nonadmin.json")
        # Wait for the denial reply, then assert no banChatMember was issued.
        ok = self._wait_for(log, lambda calls: any(c["method"] == "sendMessage" for c in calls))
        self._stop(proc)
        self.assertTrue(ok, msg=f"no reply; calls: {read_calls(log)}")

        calls = read_calls(log)
        self.assertFalse(
            any(c["method"] == "banChatMember" for c in calls),
            msg=f"non-admin must not ban; calls: {calls}",
        )
        reply = next(c for c in calls if c["method"] == "sendMessage")
        self.assertIn("not allowed", reply["json"]["text"].lower())

    def test_kick_issues_ban_then_unban(self) -> None:
        proc, log = self._start("kick_reply.json")
        ok = self._wait_for(log, lambda calls: any(c["method"] == "unbanChatMember" for c in calls))
        self._stop(proc)
        self.assertTrue(ok, msg=f"no unbanChatMember; calls: {read_calls(log)}")

        calls = read_calls(log)
        methods = [c["method"] for c in calls]
        self.assertIn("banChatMember", methods)
        self.assertIn("unbanChatMember", methods)
        # The ban must come before the unban (the kick continuation).
        self.assertLess(methods.index("banChatMember"), methods.index("unbanChatMember"))
        unban = next(c for c in calls if c["method"] == "unbanChatMember")
        self.assertTrue(unban["json"]["only_if_banned"])

    def test_ban_username_resolves_from_db(self) -> None:
        proc, log = self._start("ban_username.json")
        ok = self._wait_for(log, lambda calls: any(c["method"] == "banChatMember" for c in calls))
        self._stop(proc)
        self.assertTrue(ok, msg=f"no banChatMember; calls: {read_calls(log)}")
        ban = next(c for c in read_calls(log) if c["method"] == "banChatMember")
        # The @spammer username (user 300) was resolved from the users cache.
        self.assertEqual(ban["json"]["user_id"], 300)

    def test_purge_deletes_reply_to_command_range(self) -> None:
        proc, log = self._start("purge_range.json")
        ok = self._wait_for(log, lambda calls: any(c["method"] == "deleteMessages" for c in calls))
        self._stop(proc)
        self.assertTrue(ok, msg=f"no deleteMessages; calls: {read_calls(log)}")
        call = next(c for c in read_calls(log) if c["method"] == "deleteMessages")
        # The reply is message 1 and the command is message 6, so the range 1..6
        # is deleted in one batch — independent of what the bot observed.
        self.assertEqual(call["json"]["message_ids"], [1, 2, 3, 4, 5, 6])


if __name__ == "__main__":
    unittest.main()
