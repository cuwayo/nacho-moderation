#!/usr/bin/env python3
"""test_integration_m6.py - M6 automod and captcha end to end.

Drives the real `tgbot` against tests/mock/mock_tg_server.py over plain HTTP and
asserts the M6 paths (SPEC-ADDENDUM-TOOLCHAIN A5.3):
  * a locked media type is deleted (`deleteMessage`) while plain text is not;
  * a burst over the antiflood threshold mutes the sender (`restrictChatMember`)
    and deletes the burst messages;
  * enabling the captcha then a join restricts the member and sends an
    inline-keyboard prompt;
  * a captcha callback pressed by the wrong user is rejected.

The default run polls until SIGTERM, so each test starts the process, waits for
the expected calls, then stops it. Skipped when the binary is unavailable.

Run: python3 tests/integration/test_integration_m6.py
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
class M6IntegrationTest(unittest.TestCase):
    def _start(self, scenario: str, db_path: Path | None = None) -> tuple[subprocess.Popen, Path]:
        self.port = free_port()
        self.tmpdir = tempfile.TemporaryDirectory(prefix="nm_m6_")
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

    def test_lock_deletes_matching_media(self) -> None:
        proc, log = self._start("automod_locks.json")
        ok = self._wait_for(
            log, lambda calls: any(c["method"] == "deleteMessage" for c in calls)
        )
        self._stop(proc)
        self.assertTrue(ok, msg=f"no deleteMessage; calls: {read_calls(log)}")
        calls = read_calls(log)
        deletes = [c for c in calls if c["method"] == "deleteMessage"]
        # The locked photo (message 11) was deleted; the command was not.
        self.assertTrue(any(c["json"]["message_id"] == 11 for c in deletes),
                        msg=f"deletes: {deletes}")

    def test_antiflood_mutes_burst(self) -> None:
        proc, log = self._start("automod_antiflood.json")
        # Wait until both the mute and the burst deletions have been recorded.
        ok = self._wait_for(
            log,
            lambda calls: any(c["method"] == "restrictChatMember" for c in calls)
            and any(c["method"] == "deleteMessage" for c in calls),
        )
        self._stop(proc)
        self.assertTrue(ok, msg=f"no mute+delete; calls: {read_calls(log)}")
        calls = read_calls(log)
        mute = next(c for c in calls if c["method"] == "restrictChatMember")
        self.assertEqual(mute["json"]["user_id"], 300)
        self.assertFalse(mute["json"]["permissions"]["can_send_messages"])
        # The burst messages were deleted.
        self.assertTrue(any(c["method"] == "deleteMessage" for c in calls))

    def test_captcha_join_restricts_and_prompts(self) -> None:
        # Part 1: enable the captcha and stop, so the DB is committed.
        shared_db = Path(tempfile.mkdtemp(prefix="nm_m6_db_")) / "bot.db"
        self.addCleanup(lambda: __import__("shutil").rmtree(shared_db.parent, ignore_errors=True))
        proc1, log1 = self._start("captcha_join.json", db_path=shared_db)
        ok1 = self._wait_for(
            log1,
            lambda calls: any(
                c["method"] == "sendMessage" and c.get("json")
                and "Captcha enabled" in c["json"].get("text", "")
                for c in calls
            ),
        )
        self._stop(proc1)
        self.assertTrue(ok1, msg=f"captcha not enabled; calls: {read_calls(log1)}")

        # Part 2: a join must be challenged (restrict + inline prompt).
        proc2, log2 = self._start("captcha_join_part2.json", db_path=shared_db)
        ok2 = self._wait_for(
            log2,
            lambda calls: any(
                c["method"] == "sendMessage" and c.get("json")
                and "reply_markup" in c["json"]
                for c in calls
            ),
        )
        self._stop(proc2)
        self.assertTrue(ok2, msg=f"no captcha prompt; calls: {read_calls(log2)}")
        calls = read_calls(log2)
        # The member was restricted immediately.
        restricts = [c for c in calls if c["method"] == "restrictChatMember"]
        self.assertTrue(restricts, msg=f"no restrict; calls: {calls}")
        self.assertEqual(restricts[0]["json"]["user_id"], 300)
        self.assertFalse(restricts[0]["json"]["permissions"]["can_send_messages"])
        # The prompt carries the cap: callback data for that user.
        prompt = next(c for c in calls if c["method"] == "sendMessage"
                      and c.get("json") and "reply_markup" in c["json"])
        data = json.dumps(prompt["json"]["reply_markup"])
        self.assertIn("cap:-1001234567890:300:", data)

    def test_captcha_wrong_user_rejected(self) -> None:
        proc, log = self._start("captcha_wrong_user.json")
        ok = self._wait_for(
            log, lambda calls: any(c["method"] == "answerCallbackQuery" for c in calls)
        )
        self._stop(proc)
        self.assertTrue(ok, msg=f"no answerCallbackQuery; calls: {read_calls(log)}")
        calls = read_calls(log)
        answer = next(c for c in calls if c["method"] == "answerCallbackQuery")
        # Rejected with a toast; the challenged user (300) is not the presser (999).
        self.assertIn("not yours", answer["json"].get("text", "").lower())


if __name__ == "__main__":
    unittest.main()
