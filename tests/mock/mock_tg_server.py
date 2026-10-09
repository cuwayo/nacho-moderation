#!/usr/bin/env python3
"""mock_tg_server.py - scripted fake Telegram Bot API server (plain HTTP).

SPEC-ADDENDUM-TOOLCHAIN A5.3. Test-only dependency (Python 3 stdlib). It serves a
small set of Bot API methods over plain HTTP so integration tests can drive the
real event loop without TLS or a real token, using `api.base = http://127.0.0.1:PORT`.

Usage:
    python3 tests/mock/mock_tg_server.py --port 8081 [--scenario FILE] [--log FILE]

The default behaviour answers every `getMe` with a fixed bot identity. Scenario
JSON and fault injection (429/500/reset) are added in M2 alongside the API pool.
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

# Paths look like /bot<token>/<method>.
PATH_RE = re.compile(r"^/bot[^/]+/(?P<method>[A-Za-z]+)")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "mock-tg/0.1"

    def _respond(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _handle(self, method: str) -> None:
        match = PATH_RE.match(self.path)
        api_method = match.group("method") if match else ""
        if self.server.log_path:  # type: ignore[attr-defined]
            with open(self.server.log_path, "a", encoding="utf-8") as fh:  # type: ignore[attr-defined]
                fh.write(json.dumps({"method": api_method, "path": self.path}) + "\n")

        if api_method == "getMe":
            self._respond(200, {"ok": True, "result": DEFAULT_ME})
        elif api_method == "getUpdates":
            self._respond(200, {"ok": True, "result": []})
        else:
            self._respond(200, {"ok": False, "description": f"unknown method {api_method}"})

    def do_GET(self) -> None:  # noqa: N802 (http.server API)
        self._handle("GET")

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("Content-Length", "0"))
        if length:
            self.rfile.read(length)
        self._handle("POST")

    def log_message(self, fmt: str, *args) -> None:  # silence per-request logging
        return


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8081)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--log", default="", help="JSONL file to record received calls")
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), Handler)
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
