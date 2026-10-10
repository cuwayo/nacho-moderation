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
]

# A minimal administrator list returned by getChatAdministrators (the creator is
# id 100, an admin with restrict rights is id 200, and the bot itself is id
# 123456789 — a real bot must be an administrator to moderate, so the list
# includes it with the rights the moderation commands need).
DEFAULT_ADMINS = [
    {"status": "creator", "user": {"id": 100, "is_bot": False, "first_name": "Owner"}},
    {
        "status": "administrator",
        "user": {"id": 200, "is_bot": False, "first_name": "Admin"},
        "can_restrict_members": True,
        "can_delete_messages": True,
        "can_pin_messages": True,
    },
    {
        "status": "administrator",
        "user": {"id": DEFAULT_ME["id"], "is_bot": True, "first_name": "Mock Bot"},
        "can_restrict_members": True,
        "can_delete_messages": True,
        "can_pin_messages": True,
    },
]

# Paths look like /bot<token>/<method>.
PATH_RE = re.compile(r"^/bot[^/]+/(?P<method>[A-Za-z]+)")


class MockState:
    """Mutable state shared by all requests (single scenario run)."""

    def __init__(self) -> None:
        self.updates_served = 0
        self.scripted: list[dict] = []
        self.faults: dict[str, list[dict]] = {}
        self.recorded: list[dict] = []

    def next_update_batch(self) -> dict:
        """Return one scripted update batch, then empty batches."""
        if self.scripted:
            batch = self.scripted.pop(0)
            return {"ok": True, "result": batch}
        return {"ok": True, "result": []}

    def next_fault(self, method: str) -> dict | None:
        """Pop the next scripted fault for a method, if any."""
        faults = self.faults.get(method)
        if faults:
            return faults.pop(0)
        return None


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
        try:
            entry["json"] = json.loads(body) if body else None
        except json.JSONDecodeError:
            entry["json"] = None
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
            self._send_json(200, {"ok": True, "result": {"message_id": 1}})
        elif api_method == "setMyCommands":
            self._send_json(200, {"ok": True, "result": True})
        elif api_method in ("banChatMember", "unbanChatMember", "restrictChatMember"):
            self._send_json(200, {"ok": True, "result": True})
        elif api_method == "deleteMessage":
            self._send_json(200, {"ok": True, "result": True})
        elif api_method == "deleteMessages":
            self._send_json(200, {"ok": True, "result": True})
        elif api_method in ("pinChatMessage", "unpinChatMessage"):
            self._send_json(200, {"ok": True, "result": True})
        else:
            self._send_json(200, {"ok": False, "description": f"unknown method {api_method}"})

    def do_GET(self) -> None:  # noqa: N802 (http.server API)
        self._handle("GET", b"")

    def do_POST(self) -> None:  # noqa: N802
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
    print(f"mock-tg listening on http://{args.host}:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
