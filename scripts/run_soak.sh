#!/usr/bin/env bash
# run_soak.sh - replay a sustained update load against the mock Bot API server
# and check the bot stays healthy (no crash, bounded RSS growth).
#
# Usage: scripts/run_soak.sh [seconds] [rate]
#   seconds  duration of the soak (default: 60)
#   rate     updates per second the mock feeds (default: 200)
#
# The mock server's --soak mode queues synthetic supergroup messages; the bot's
# getUpdates long-poll drains them. The script runs the real bot against the mock
# and samples RSS and open file descriptors, failing on unbounded growth.

set -euo pipefail

SECONDS_TO_RUN="${1:-${SOAK_SECONDS:-60}}"
RATE="${2:-${SOAK_RATE:-200}}"
RSS_GROWTH_PCT="${SOAK_RSS_GROWTH_PCT:-15}"

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

PORT="$(free_port)"
LOG="$(mktemp)"
CALLS="$(mktemp)"
trap 'rm -f "$LOG" "$CALLS"' EXIT

echo "soak: starting mock Bot API server on 127.0.0.1:${PORT} (${SECONDS_TO_RUN}s @ ${RATE}/s)"
python3 tests/mock/mock_tg_server.py --port "$PORT" --soak "$((SECONDS_TO_RUN + 30))" --rate "$RATE" \
  --log "$CALLS" >"$LOG" 2>&1 &
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

export MODE=polling
export API_BASE="http://127.0.0.1:${PORT}"
export BOT_TOKEN="123456:TEST"
export DB_PATH="$(mktemp -u)"
export SUDO_IDS=""
export LOG_LEVEL=info

echo "soak: starting bot"
uv run nacho-moderator >"$LOG.bot" 2>&1 &
BOT_PID=$!

sample_rss() { awk '/VmRSS/{print $2}' "/proc/$BOT_PID/status" 2>/dev/null || echo 0; }
sample_fds() { ls "/proc/$BOT_PID/fd" 2>/dev/null | wc -l || echo 0; }

sleep 5
FIRST_RSS="$(sample_rss)"
FIRST_FDS="$(sample_fds)"
sleep "$SECONDS_TO_RUN"
LAST_RSS="$(sample_rss)"
LAST_FDS="$(sample_fds)"

if ! kill -0 "$BOT_PID" 2>/dev/null; then
  echo "soak: FAIL - the bot exited during the soak" >&2
  cat "$LOG.bot" >&2 || true
  kill "$MOCK_PID" 2>/dev/null || true
  exit 1
fi

kill -TERM "$BOT_PID" 2>/dev/null || true
wait "$BOT_PID" 2>/dev/null || true
kill "$MOCK_PID" 2>/dev/null || true

CALL_COUNT="$(wc -l < "$CALLS" 2>/dev/null || echo 0)"
echo "soak: ${CALL_COUNT} API calls recorded; RSS ${FIRST_RSS}kB -> ${LAST_RSS}kB; FDs ${FIRST_FDS} -> ${LAST_FDS}"

if [ "$FIRST_RSS" -gt 0 ]; then
  GROWTH=$(( (LAST_RSS - FIRST_RSS) * 100 / FIRST_RSS ))
  if [ "$GROWTH" -gt "$RSS_GROWTH_PCT" ]; then
    echo "soak: FAIL - RSS grew ${GROWTH}% (> ${RSS_GROWTH_PCT}%)" >&2
    exit 1
  fi
fi

if [ "$CALL_COUNT" -eq 0 ]; then
  echo "soak: FAIL - no API calls were recorded" >&2
  exit 1
fi

echo "soak: PASS"
