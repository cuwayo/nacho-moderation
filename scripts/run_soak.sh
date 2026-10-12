#!/usr/bin/env bash
# run_soak.sh - replay a sustained update load against the mock Bot API server
# and check the bot stays healthy.
#
# Usage: scripts/run_soak.sh [mode] [--seconds N] [--rate R]
#   mode       "smoke" (30 s, every PR) or "nightly" (600 s @ 1000/s, default)
#
# Environment overrides:
#   SOAK_SECONDS, SOAK_RATE, SOAK_MIN_CALLS
#
# The bot's getUpdates long-poll drains synthetic messages queued by the mock's
# --soak mode. The bot records per-update handler latency and event-loop lag in
# a histogram and logs a `soak-metrics ...` summary on shutdown (and on SIGUSR1).
# This script samples RSS and FD count every 5 s, then hands the artifacts to
# scripts/soak_check.py, which owns the pass/fail thresholds.

set -euo pipefail

MODE="${1:-nightly}"
shift || true

SECONDS_TO_RUN="${SOAK_SECONDS:-}"
RATE="${SOAK_RATE:-}"
MIN_CALLS="${SOAK_MIN_CALLS:-1}"

# Parse optional --seconds/--rate overrides.
while [ $# -gt 0 ]; do
  case "$1" in
    --seconds) SECONDS_TO_RUN="$2"; shift 2 ;;
    --rate) RATE="$2"; shift 2 ;;
    --min-calls) MIN_CALLS="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

# Mode defaults: the smoke mode is short and uses asyncio debug to catch slow
# callbacks and unclosed resources; the nightly run is the full load.
DEBUG_ENV=()
case "$MODE" in
  smoke)
    SECONDS_TO_RUN="${SECONDS_TO_RUN:-30}"
    RATE="${RATE:-1000}"
    DEBUG_ENV=(PYTHONASYNCIODEBUG=1)
    PYTHON_DEBUG_FLAGS=(-X dev -W error::ResourceWarning)
    ;;
  nightly)
    SECONDS_TO_RUN="${SECONDS_TO_RUN:-600}"
    RATE="${RATE:-1000}"
    PYTHON_DEBUG_FLAGS=()
    ;;
  *)
    echo "unknown mode: $MODE (expected 'smoke' or 'nightly')" >&2
    exit 2
    ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

free_port() {
  python3 - <<'PY'
import socket
with socket.socket() as s:
    s.bind(("127.0.0.1", 0))
    print(s.getsockname()[1])
PY
}

WORKDIR="$(mktemp -d)"
PORT="$(free_port)"
LOG="$WORKDIR/bot.log"
CALLS="$WORKDIR/calls.jsonl"
SAMPLES="$WORKDIR/samples.txt"
CONFIG="$WORKDIR/config.toml"
trap 'rm -rf "$WORKDIR"' EXIT

DB="$WORKDIR/bot.db"
cat >"$CONFIG" <<EOF
bot_token = "123456:TEST"
mode = "polling"

[api]
base = "http://127.0.0.1:${PORT}"

[db]
path = "${DB}"

[polling]
timeout = 1

[log]
level = "info"
EOF
chmod 600 "$CONFIG"

echo "soak: mode=$MODE ${SECONDS_TO_RUN}s @ ${RATE}/s (min calls ${MIN_CALLS})"
python3 tests/mock/mock_tg_server.py --port "$PORT" --soak "$((SECONDS_TO_RUN + 30))" --rate "$RATE" \
  --log "$CALLS" >"$WORKDIR/mock.log" 2>&1 &
MOCK_PID=$!

# Wait for the mock to accept connections.
for _ in $(seq 1 50); do
  if python3 - "$PORT" <<'PY'
import socket, sys
port = int(sys.argv[1])
try:
    with socket.create_connection(("127.0.0.1", port), timeout=0.2):
        pass
except OSError:
    sys.exit(1)
PY
  then break; fi
  sleep 0.1
done

echo "soak: starting bot"
env "${DEBUG_ENV[@]}" uv run python "${PYTHON_DEBUG_FLAGS[@]}" -m nacho_moderator --config "$CONFIG" \
  >"$LOG" 2>&1 &
BOT_PID=$!

sample() {
  local rss fds
  rss="$(awk '/VmRSS/{print $2}' "/proc/$BOT_PID/status" 2>/dev/null || echo 0)"
  fds="$(ls "/proc/$BOT_PID/fd" 2>/dev/null | wc -l || echo 0)"
  echo "${rss:-0} ${fds:-0}" >>"$SAMPLES"
}

# Wait for the bot to open its DB (rough proxy for "up"), then sample every 5 s.
sleep 2
ELAPSED=0
while [ "$ELAPSED" -lt "$SECONDS_TO_RUN" ]; do
  sample
  sleep 5
  ELAPSED=$((ELAPSED + 5))
  if ! kill -0 "$BOT_PID" 2>/dev/null; then break; fi
done
sample

BOT_EXIT=0
if kill -0 "$BOT_PID" 2>/dev/null; then
  # Ask for a metrics dump, then terminate.
  kill -USR1 "$BOT_PID" 2>/dev/null || true
  sleep 1
  kill -TERM "$BOT_PID" 2>/dev/null || true
fi
wait "$BOT_PID" || BOT_EXIT=$?
kill "$MOCK_PID" 2>/dev/null || true
wait "$MOCK_PID" 2>/dev/null || true

CALL_COUNT="$(wc -l < "$CALLS" 2>/dev/null || echo 0)"
echo "soak: ${CALL_COUNT} API calls recorded; $(wc -l < "$SAMPLES") samples"

uv run python scripts/soak_check.py \
  --bot-log "$LOG" \
  --samples "$SAMPLES" \
  --calls "$CALLS" \
  --exit-code "$BOT_EXIT" \
  --min-calls "$MIN_CALLS"
