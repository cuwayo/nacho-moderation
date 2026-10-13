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

The soak gate uses these budgets, measured inside the bot (not from the harness).
Loop lag is split into a **warm-up** window (the first 60 s, matching the RSS
warm-up) and **steady state**, so a startup transient does not fail the
steady-state budgets while a genuine startup hang still does:

| Metric | Budget | Rationale |
| --- | --- | --- |
| Handler latency p99 | **≤ 50 ms** | Well under the ~100 ms threshold where an interaction starts to feel slow; leaves headroom above the observed p99 of ~1 ms so only a real regression trips it. |
| Loop lag, warm-up (first 60 s) | max **≤ 1000 ms** | A stalled startup (a hung first batch, a slow DB open) must still fail; the steady-state budgets do not apply during warm-up. |
| Loop lag, steady state | p99 **≤ 100 ms**, max **≤ 250 ms** | The p99 is the design bound: a loop that blocks 100 ms at the 99th percentile delays a perceptible fraction of callbacks. The max is a *regression guard* (a hard ceiling), not a design target — a Python loop processing a 100-update batch stalls briefly by construction. |
| Achieved update rate | **≥ 80 % of requested** | The gate derives the minimum handler-sample count from the run itself (`0.8 * rate * seconds`) rather than a fixed default, so a run that quietly served a fraction of the load fails. |
| RSS growth after warm-up | **≤ 10 %** | A steady-state process must not leak. The 60 s warm-up excludes allocator/interning warm-up. |
| FD count | **flat** (slope ≤ 0.5/sample) | A climbing FD count means a leaked socket or file. |

The C implementation's `io_uring` stall budget (milliseconds) does **not** apply;
this ADR supersedes it for the Python port. The numbers above are the documented
budget the soak test enforces.

**These timing budgets are only meaningful with production flags.** The smoke run
(`PYTHONASYNCIODEBUG=1 -X dev`) checks hygiene — tracebacks, un-awaited
coroutines, resource warnings — and the achieved-rate floor; it reports the timing
numbers without asserting them. The nightly run enforces the timing budgets.

### Load and batching (revised 2026-10-13)

An earlier version of the mock served **one** update per `getUpdates`
(`SOAK_BATCH = 1`). With aiogram polling that caps the achieved rate at the poll
round-trip rate (~26 updates/s), so a "1000/s" run actually carried ~3 % of its
stated load while the gate passed (its minimum was a fixed default of 1). The
mock now serves up to **100 updates per reply** (Telegram's real default),
configurable with `--soak-batch`, and the gate derives its floor from the run.
The batch stays at 100; shrinking it to pass the gate would hide the real
per-iteration cost.

Measured on the development host (Linux, CPython 3.12.14, single chat,
batch = 100; `uptime_seconds` is the bot's own processing window, and the lag
numbers are split by window):

| Run | Achieved rate | Handler p50 / p99 | Steady lag p99 / max |
| --- | --- | --- | --- |
| 300 s @ 1000/s, before the GC freeze | ~985/s (99 %) | 0.5 ms / 1 ms | 54.8 ms / **138.9 ms** |
| 300 s @ 1000/s, after the GC freeze | **~983/s (98 %)** | 0.5 ms / 1 ms | **36.5 ms / 70.3 ms** |
| 120 s @ 1000/s, GC disabled after startup (attribution only) | — | 0.5 ms / 0.5 ms | 46.2 ms / 75.7 ms |
| 30 s @ 1000/s, smoke (`PYTHONASYNCIODEBUG=1 -X dev`) | ~830/s (83 %) | 0.5 ms / — | — |

The "before" run had five post-warm-up lag events at or over 100 ms (139 ms at
t=141 s, 128 ms at t=208 s, 127 ms at t=218 s, 127 ms at t=248 s, 125 ms at
t=84 s). The "after" run has **none** — its largest post-warm-up event is 70 ms.
The `handler-time fraction` note is an *aggregate* (handlers run as concurrent
tasks, so it sums to ~0.23 here over the window; it is not wall-clock loop
occupancy).

**Handler latency is comfortably within budget in production settings** (p99
~1 ms). Loop lag comes from two sources, both measured rather than assumed:

1. **Per-iteration batch cost.** `aiogram` validates each update (pydantic) and
   dispatches it through the middleware chain synchronously before yielding, so a
   burst of 100 updates costs a few tens of milliseconds of loop time. This is
   inherent to a 100-update reply; the batch stays at Telegram's real default.
2. **Generational-GC pauses.** The initial measurements showed isolated >100 ms
   loop-lag spikes *after* warm-up (e.g. 139 ms at t=141 s, 128 ms at t=208 s) at
   ~10 s intervals. Instrumenting `gc.callbacks` attributed them to **gen-2
   collections** — the collector walking the large, stable long-lived object graph
   (the aiogram bot, dispatcher, services, caches, and every pydantic type). With
   gen-2 GC disabled after startup the steady-state max fell from ~174 ms to
   ~76 ms with **zero** post-warm-up events ≥ 100 ms, confirming the cause. This
   was fixed in the bot (not by raising the budget): `freeze_gc_after_startup()`
   runs `gc.collect()` then `gc.freeze()` once the long-lived graph exists, moving
   it to CPython's permanent generation so the collector never scans it again,
   while per-update objects stay collectable.

With the freeze in place the steady-state max drops well under the ceiling; the
250 ms ceiling remains a regression guard with headroom, and the p99 (the design
bound) reflects the residual per-batch cost.

**The smoke run's numbers are dominated by `PYTHONASYNCIODEBUG`.** With asyncio
debug on, every coroutine creation is traced and the loop's slow-callback
detector adds instrumentation, inflating latency and loop lag by an order of
magnitude. That is why the smoke run asserts hygiene and the achieved-rate floor
only.

<!-- MEASURED-RUNS -->

## Consequences

- **No `io_uring`, no sanitizers.** Memory-safety and stall guarantees that a C
  build got from ASan/TSan and a raw ring are replaced by the debug allocator
  (`PYTHONMALLOC=debug` via `-X dev`) and `PYTHONASYNCIODEBUG`, run in the smoke
  mode so slow callbacks and unclosed resources fail the run. That smoke run
  checks hygiene and the achieved-rate floor; the timing budgets run in the
  nightly (production-flag) job.
- **A looser stall budget** (100 ms steady-state p99 vs. a few ms) is accepted.
  The trade is Python's ergonomics and safety for a coarser worst-case scheduling
  delay.
- **The steady-state max is a regression guard, not a design target.** A Python
  loop processing a 100-update batch stalls briefly by construction; the 250 ms
  ceiling catches a real regression (a blocking call, a runaway handler) without
  failing on the batch's inherent cost. The p99 is the number that reflects
  user-visible responsiveness.
- **The long-lived object graph is frozen from the cyclic GC.** `run_bot` calls
  `gc.freeze()` once, after startup, so gen-2 collections no longer walk the
  stable bot/dispatcher/service graph — the source of the >100 ms spikes measured
  at 1000/s. Per-update objects stay collectable, so cyclic garbage is still
  reclaimed. The `gc.collect()` immediately before the freeze drains any startup
  garbage first.
- **The warm-up window keeps a hard ceiling.** A startup hang (a stalled first
  batch, a slow DB open) still fails, at 1000 ms, so moving the budgets to
  steady state does not hide a startup regression.
- **The smoke run asserts hygiene, not timing.** `PYTHONASYNCIODEBUG` traces
  every coroutine and adds slow-callback instrumentation, inflating timing by an
  order of magnitude. The smoke run's value is the debug checks themselves
  (warnings as errors, unclosed resources, slow callbacks) plus the achieved-rate
  floor; the timing budgets are meaningful in the production (nightly) settings.
- **The GIL** bounds concurrency to one thread for Python bytecode; `aiosqlite`
  and the TLS stack release it around blocking I/O, which is why a 1000 update/s
  load still keeps the loop responsive.
- The budgets live in `scripts/soak_check.py` as the single source of truth and
  are enforced on every PR (30 s smoke: hygiene) and nightly (10 min: timing).

## Alternatives

- **Keep the C stall budget (a few ms).** Rejected: it would fail on ordinary
  interpreter and GC pauses, turning the gate into noise.
- **No budgets; only "did it crash".** Rejected: a leak or a slow handler would
  pass unnoticed until it caused an outage.
- **Keep the C implementation.** Rejected — see
  [ADR 0001](0001-python-asyncio-aiogram.md).
