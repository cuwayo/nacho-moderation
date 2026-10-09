#!/usr/bin/env bash
# run_soak.sh - 10-minute load/soak harness (SPEC-ADDENDUM-TOOLCHAIN A5.5).
#
# Replays 1,000 updates/s against the Python mock Telegram server under the ASan
# build, then checks p50/p99 latency, loop stalls, RSS/FD flatness, and LSan.
#
# M0 note: the mock server, event loop, and --fake-time-offset flag arrive in
# M1-M7; this placeholder documents the intended invocation and exits 0 so the
# nightly CI job is wired but not yet enforcing.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "run_soak: not yet runnable - requires the mock server (A5.3) and the event"
echo "run_soak: loop (M1+). Intended flow once available:"
echo "  1. python3 tests/mock/mock_tg_server.py --scenario soak --port 8080 &"
echo "  2. build/asan/tgbot --config tests/fixtures/soak.toml --fake-time-offset 0"
echo "  3. replay 1000 updates/s for 600 s; collect latency + RSS/FD + LSan."
exit 0
