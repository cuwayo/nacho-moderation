# ADR 0002: Loop-lag and handler-latency budgets for the Python bot

- **Status:** Accepted
- **Date:** 2026-10-12
- **Related:** [ADR 0001](0001-python-asyncio-aiogram.md)

## Context

The soak test (`scripts/run_soak.sh`, `scripts/soak_check.py`) must fail a run
that shows the bot is not healthy under sustained load. The original C
implementation (see the `c-implementation-final` tag) ran on `io_uring` and had a
tight per-iteration stall budget — a few milliseconds — because a single
threaded event loop serviced raw syscalls directly.

Python is not C. The interpreter, the garbage collector, and the `asyncio`
scheduler all add overhead that a C loop does not pay, and `aiogram`'s dispatcher
runs several middleware layers and awaits `aiohttp` I/O per update. Holding the
port to the C numbers would make the gate flaky rather than meaningful: it would
fail on interpreter noise instead of on a real regression.

The bot's real constraint is user-visible responsiveness. A moderator command
must feel instant; a background sweep or a burst of updates must not stall the
loop long enough to delay other updates noticeably.

## Decision

The soak gate uses these budgets, measured inside the bot (not from the harness):

| Metric | Budget | Rationale |
| --- | --- | --- |
| Handler latency p99 | **≤ 50 ms** | Well under the ~100 ms threshold where an interaction starts to feel slow; leaves headroom above the observed p99 of ~2 ms so only a real regression trips it. |
| Event-loop lag (max and p99) | **≤ 100 ms** | A loop blocked for 100 ms delays every pending callback by that much. Below the budget, a user cannot perceive the delay; above it, a burst of synchronous work (a bad regex, a blocking call) is the likely cause. |
| RSS growth after warm-up | **≤ 10 %** | A steady-state process must not leak. The 60 s warm-up excludes allocator/interning warm-up. |
| FD count | **flat** (slope ≤ 0.5/sample) | A climbing FD count means a leaked socket or file. |

The C implementation's `io_uring` stall budget (milliseconds) does **not** apply;
this ADR supersedes it for the Python port. The numbers above are the documented
budget the soak test enforces.

## Consequences

- **No `io_uring`, no sanitizers.** Memory-safety and stall guarantees that a C
  build got from ASan/TSan and a raw ring are replaced by the debug allocator
  (`PYTHONMALLOC=debug` via `-X dev`) and `PYTHONASYNCIODEBUG`, run in the smoke
  mode so slow callbacks and unclosed resources fail the run.
- **A looser stall budget** (100 ms vs. a few ms) is accepted. The trade is
  Python's ergonomics and safety for a coarser worst-case scheduling delay.
- **The GIL** bounds concurrency to one thread for Python bytecode; `aiosqlite`
  and the TLS stack release it around blocking I/O, which is why a 1000 update/s
  load still keeps the loop responsive.
- The budgets live in `scripts/soak_check.py` as the single source of truth and
  are enforced on every PR (30 s smoke) and nightly (10 min).

## Alternatives

- **Keep the C stall budget (a few ms).** Rejected: it would fail on ordinary
  interpreter and GC pauses, turning the gate into noise.
- **No budgets; only "did it crash".** Rejected: a leak or a slow handler would
  pass unnoticed until it caused an outage.
- **Keep the C implementation.** Rejected — see
  [ADR 0001](0001-python-asyncio-aiogram.md).
