#!/usr/bin/env bash
# check_blocking.sh - assert the main thread performs no blocking syscalls
# (SPEC-ADDENDUM-TOOLCHAIN A4, A5.5; base SPEC section 11.7).
#
# The bot's main thread must never call blocking read/connect/poll/futex-wait or
# touch SQLite/file I/O directly; all such work is deferred to worker threads.
#
# Usage: scripts/check_blocking.sh <binary> [args...]
#
# Startup I/O (dynamic-linker library loads, config read) legitimately happens on
# the main thread before the event loop runs. To avoid flagging it without
# weakening the forbidden-syscall list, the binary prints a "loop: ready" sentinel
# to stderr once the loop is in steady state; only syscalls after that point are
# checked. The sentinel write itself appears in the trace, so the two are aligned
# without timestamps.
set -euo pipefail

if [ "$#" -eq 0 ]; then
    echo "check_blocking: no binary given; pass a built tgbot (e.g. build/asan/tgbot)."
    exit 2
fi

binary="$1"
shift || true

if [ ! -x "${binary}" ]; then
    echo "check_blocking: '${binary}' is not executable" >&2
    exit 2
fi

# Blocking syscalls that must not appear on the main thread after startup.
forbidden_re='\b(read|pread64|readv|connect|poll|ppoll|select|pselect6|futex|nanosleep|openat|fsync)\b'

if ! command -v strace >/dev/null 2>&1; then
    echo "check_blocking: strace unavailable; skipping (enforced in CI)" >&2
    exit 0
fi

trace_out="$(mktemp)"
trap 'rm -f "${trace_out}"' EXIT

# -f follows threads; -tt is not needed because we align on the sentinel write.
strace -f -qq -e trace=network,file,desc -o "${trace_out}" "${binary}" "$@" >/dev/null 2>&1 || true

main_pid="$(head -n1 "${trace_out}" | sed -E 's/^([0-9]+).*/\1/')"
if [ -z "${main_pid}" ]; then
    echo "check_blocking: could not determine the main thread pid" >&2
    exit 2
fi

# Line number of the sentinel write on the main thread, if present.
start_line="$(grep -nE "^${main_pid} .*write\(2, \"loop: ready" "${trace_out}" | head -n1 | cut -d: -f1 || true)"
if [ -z "${start_line}" ]; then
    echo "check_blocking: sentinel 'loop: ready' not found in trace; is this a real loop binary?" >&2
    exit 2
fi

# Inspect only the main thread's syscalls after the sentinel.
if tail -n "+${start_line}" "${trace_out}" | grep -E "^${main_pid} " | grep -qE "${forbidden_re}"; then
    echo "check_blocking: FAIL - blocking syscall on main thread (pid ${main_pid}):" >&2
    tail -n "+${start_line}" "${trace_out}" | grep -E "^${main_pid} " | grep -E "${forbidden_re}" >&2 || true
    exit 1
fi

echo "check_blocking: OK (main thread clean after startup)"
