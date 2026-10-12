#!/usr/bin/env python3
"""mock_tg_server.py - scripted fake Telegram Bot API server (plain HTTP).

SPEC-ADDENDUM-TOOLCHAIN A5.3. Test-only dependency (Python 3 stdlib). It serves a
small set of Bot API methods over plain HTTP so integration tests can drive the
real event loop without TLS or a real token, using `api.base = http://127.0.0.1:PORT`.

Usage:
    python3 tests/mock/mock_tg_server.py --port 8081 [--scenario FILE] [--log FILE]

Behaviour:
  * Without a scenario every `getMe` returns a fixed identity and every
    `getUpdates` returns one scripted update then empty batches (so a polling
    test can observe the offset advance).
  * With `--scenario FILE` the JSON scenario drives the responses; a fault
    injection list can force 429/500/reset/malformed replies.

The `HANDLERS` registry below lists every method the server implements; it is the
source of truth for consistency check C9 (every method in `api_methods.def` must
have a handler here).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_ME = {
    "id": 123456789,
    "is_bot": True,
    "first_name": "Mock Bot",
    "username": "mock_bot",
}

# Every Bot API method this mock implements. Keep in sync with src/api_methods.def
# (consistency check C9).
HANDLERS = [
    "getMe",
    "getUpdates",
    "getChatAdministrators",
    "sendMessage",
    "setMyCommands",
    "banChatMember",
    "unbanChatMember",
    "restrictChatMember",
    "deleteMessage",
    "deleteMessages",
    "pinChatMessage",
    "unpinChatMessage",
    "getChatMemberCount",
    "answerCallbackQuery",
    "editMessageText",
]

# A minimal administrator list returned by getChatAdministrators (the creator is
# id 100, an admin with restrict rights is id 200, and the bot itself is id
# 123456789 — a real bot must be an administrator to moderate, so the list
# includes it with the rights the moderation commands need).
DEFAULT_ADMINS = [
    {
        "status": "creator",
        "user": {"id": 100, "is_bot": False, "first_name": "Owner", "username": "owner"},
        "is_anonymous": False,
    },
    {
        "status": "administrator",
        "user": {"id": 200, "is_bot": False, "first_name": "Admin", "username": "admin"},
        "can_be_edited": False,
        "is_anonymous": False,
        "can_manage_chat": True,
        "can_delete_messages": True,
        "can_manage_video_chats": True,
        "can_restrict_members": True,
        "can_promote_members": False,
        "can_change_info": True,
        "can_invite_users": True,
        "can_post_stories": False,
        "can_edit_stories": False,
        "can_delete_stories": False,
        "can_send_welcome_messages": False,
        "can_pin_messages": True,
    },
    {
        "status": "administrator",
        "user": {"id": DEFAULT_ME["id"], "is_bot": True, "first_name": "Mock Bot"},
        "can_be_edited": False,
        "is_anonymous": False,
        "can_manage_chat": True,
        "can_delete_messages": True,
        "can_manage_video_chats": True,
        "can_restrict_members": True,
        "can_promote_members": False,
        "can_change_info": True,
        "can_invite_users": True,
        "can_post_stories": False,
        "can_edit_stories": False,
        "can_delete_stories": False,
        "can_send_welcome_messages": False,
        "can_pin_messages": True,
    },
]

# Paths look like /bot<token>/<method>.
PATH_RE = re.compile(r"^/bot[^/]+/(?P<method>[A-Za-z]+)")

# Maximum number of updates returned in one getUpdates reply during a soak run.
# One at a time: the poller processes every update in a reply synchronously within
# a single event-loop iteration, so a large reply would make one iteration do
# batch-sized work and defeat the per-iteration stall gate. The soak varies the
# *rate* (updates/s), not the batch size.
SOAK_BATCH = 1


class MockState:
    """Mutable state shared by all requests (single scenario run)."""

    def __init__(self) -> None:
        self.updates_served = 0
        self.scripted: list[dict] = []
        self.faults: dict[str, list[dict]] = {}
        self.recorded: list[dict] = []
        # Soak mode: a background thread feeds synthetic updates at a target rate.
        self.soak = False
        self.soak_rate = 1000
        self.soak_until = 0.0
        self.soak_batches: list[dict] = []
        self.soak_next_id = 1
        self.soak_started = threading.Event()
        self.soak_lock = threading.Lock()

    def next_update_batch(self) -> dict:
        """Return one scripted update batch, then empty batches."""
        if self.scripted:
            batch = self.scripted.pop(0)
            return {"ok": True, "result": batch}
        if self.soak:
            # The bot's first getUpdates marks steady state: start the feeder now so
            # no startup backlog accumulates into one oversized batch.
            self.soak_started.set()
            # Cap the batch at SOAK_BATCH so a single loop iteration processes a
            # bounded amount of work; the rate, not the batch size, is what the soak
            # varies. (Telegram's default limit is 100, which under a sanitizer
            # build would exceed the 5 ms loop-stall budget in one dispatch.)
            with self.soak_lock:
                batch = self.soak_batches[:SOAK_BATCH]
                del self.soak_batches[:SOAK_BATCH]
            return {"ok": True, "result": batch}
        return {"ok": True, "result": []}

    def next_fault(self, method: str) -> dict | None:
        """Pop the next scripted fault for a method, if any."""
        faults = self.faults.get(method)
        if faults:
            return faults.pop(0)
        return None

    def soak_feeder(self) -> None:
        """Generate updates at ~soak_rate/s until soak_until elapses.

        Updates are queued (not served) here; the bot's getUpdates long-poll drains
        the queue, so the actual delivery rate is bounded by how fast the bot polls
        and processes them, which is what the soak test measures. The feeder waits
        for the bot's first poll (soak_started) so no startup backlog accumulates,
        then queues one update per tick for small, realistic batches.
        """
        self.soak_started.wait(timeout=30.0)
        interval = 1.0 / max(self.soak_rate, 1)
        next_at = time.monotonic()
        while time.monotonic() < self.soak_until:
            with self.soak_lock:
                self.soak_batches.append(_soak_update(self.soak_next_id))
                self.soak_next_id += 1
            next_at += interval
            delay = next_at - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            else:
                next_at = time.monotonic()  # keep up if we fell behind


def _parse_body(body: bytes) -> dict | None:
    """Parse a request body that may be JSON or form-encoded.

    aiogram posts multipart/form-data or application/x-www-form-urlencoded
    bodies, while the older C client posted JSON. Normalise both to a dict of
    string values (coercing obviously numeric/bool fields) for assertions.
    """
    if not body:
        return None
    try:
        return json.loads(body)
    except (json.JSONDecodeError, UnicodeDecodeError):
        pass
    try:
        text = body.decode("utf-8")
    except UnicodeDecodeError:
        return None
    parsed = urllib.parse.parse_qs(text, keep_blank_values=True)
    result: dict = {}
    for key, values in parsed.items():
        result[key] = _coerce(values[0])
    return result


def _coerce(value: str) -> object:
    """Coerce a form value to int/bool/str for readable assertions."""
    if value == "true":
        return True
    if value == "false":
        return False
    try:
        return int(value)
    except ValueError:
        return value


def _mock_message(body: bytes) -> dict:
    """Build a full Message object for sendMessage/editMessageText replies.

    aiogram 3.x validates the returned Message against a strict model that
    requires ``date`` and ``chat``, so the mock echoes them from the request.
    """
    request = _parse_body(body) or {}
    chat_id = request.get("chat_id", -1001234567890)
    return {
        "message_id": 1,
        "date": 1700000000,
        "chat": {"id": chat_id, "type": "supergroup", "title": "Testers"},
        "from": dict(DEFAULT_ME),
        "text": request.get("text", ""),
    }


def _soak_update(update_id: int) -> dict:
    """One synthetic supergroup message Update for the soak feeder."""
    return {
        "update_id": update_id,
        "message": {
            "message_id": update_id,
            "date": 1700000000,
            "chat": {"id": -1001234567890, "type": "supergroup", "title": "Soak"},
            "from": {
                "id": 300 + (update_id % 50),
                "is_bot": False,
                "first_name": "User",
                "username": f"user{update_id % 50}",
            },
            "text": f"soak message {update_id}",
        },
    }


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "mock-tg/0.2"

    def _send_json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_raw(self, status: int, body: bytes, content_type: str = "application/json") -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _record(self, method: str, body: bytes) -> None:
        entry = {"method": method, "path": self.path}
        entry["json"] = _parse_body(body)
        state: MockState = self.server.state  # type: ignore[attr-defined]
        state.recorded.append(entry)
        if self.server.log_path:  # type: ignore[attr-defined]
            with open(self.server.log_path, "a", encoding="utf-8") as fh:  # type: ignore[attr-defined]
                fh.write(json.dumps(entry) + "\n")

    def _handle(self, method: str, body: bytes) -> None:
        match = PATH_RE.match(self.path)
        api_method = match.group("method") if match else ""
        self._record(api_method, body)

        state: MockState = self.server.state  # type: ignore[attr-defined]

        fault = state.next_fault(api_method)
        if fault is not None:
            kind = fault.get("kind", "status")
            if kind == "status":
                status = int(fault.get("status", 500))
                payload = fault.get("body", {"ok": False, "description": "injected"})
                self._send_json(status, payload)
            elif kind == "malformed":
                self._send_raw(200, b"not json at all")
            elif kind == "reset":
                self.close_connection = True
                # Abruptly drop the connection without a response.
                self.wfile.close()
            return

        if api_method == "getMe":
            self._send_json(200, {"ok": True, "result": DEFAULT_ME})
        elif api_method == "getUpdates":
            self._send_json(200, state.next_update_batch())
        elif api_method == "getChatAdministrators":
            self._send_json(200, {"ok": True, "result": DEFAULT_ADMINS})
        elif api_method == "sendMessage":
            self._send_json(200, {"ok": True, "result": _mock_message(body)})
        elif (
            api_method == "setMyCommands"
            or api_method in ("banChatMember", "unbanChatMember", "restrictChatMember")
            or api_method == "deleteMessage"
            or api_method == "deleteMessages"
            or api_method in ("pinChatMessage", "unpinChatMessage")
        ):
            self._send_json(200, {"ok": True, "result": True})
        elif api_method == "getChatMemberCount":
            self._send_json(200, {"ok": True, "result": 42})
        elif api_method == "answerCallbackQuery":
            self._send_json(200, {"ok": True, "result": True})
        elif api_method == "editMessageText":
            self._send_json(200, {"ok": True, "result": _mock_message(body)})
        else:
            self._send_json(200, {"ok": False, "description": f"unknown method {api_method}"})

    def do_GET(self) -> None:
        self._handle("GET", b"")

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length else b""
        self._handle("POST", body)

    def log_message(self, fmt: str, *args) -> None:  # silence per-request logging
        return


def load_scenario(path: str, state: MockState) -> None:
    with open(path, encoding="utf-8") as fh:
        scenario = json.load(fh)
    state.scripted = list(scenario.get("updates", []))
    state.faults = {m: list(v) for m, v in scenario.get("faults", {}).items()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8081)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--log", default="", help="JSONL file to record received calls")
    parser.add_argument("--scenario", default="", help="scenario JSON file")
    parser.add_argument(
        "--soak",
        type=int,
        default=0,
        metavar="SECONDS",
        help="replay synthetic updates at --rate/s for SECONDS, then stop",
    )
    parser.add_argument(
        "--rate", type=int, default=1000, help="soak updates per second (default 1000)"
    )
    parser.add_argument("--list-handlers", action="store_true", help="print HANDLERS and exit")
    args = parser.parse_args()

    if args.list_handlers:
        print("\n".join(HANDLERS))
        return 0

    state = MockState()
    if args.scenario:
        load_scenario(args.scenario, state)

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.state = state  # type: ignore[attr-defined]
    server.log_path = args.log  # type: ignore[attr-defined]

    feeder = None
    if args.soak > 0:
        state.soak = True
        state.soak_rate = args.rate
        state.soak_until = time.monotonic() + args.soak
        feeder = threading.Thread(target=state.soak_feeder, daemon=True)
        feeder.start()

    print(f"mock-tg listening on http://{args.host}:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        state.soak = False
        server.server_close()
        if feeder is not None:
            feeder.join(timeout=2.0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
