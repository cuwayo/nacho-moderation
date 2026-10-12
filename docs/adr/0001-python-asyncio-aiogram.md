# ADR 0001: Rewrite the bot in Python with asyncio and aiogram

- **Status:** Accepted
- **Date:** 2026-10-12
- **Related:** [ADR 0002](0002-python-loop-lag-budget.md)

## Context

The bot was originally implemented in C11 on a single-threaded `io_uring` event
loop, with OpenSSL memory-BIO TLS, a hand-written HTTP/1.1 client, a SQLite
worker thread signalled through an `eventfd`, and X-macro `.def` tables as the
single source of truth. That implementation reached milestone M7 (webhook mode,
graceful shutdown, packaging) and is preserved at the
**`c-implementation-final`** git tag (commit `282ab7c`).

Maintaining the C implementation cost far more than the bot's scope warranted:
hand-rolled TLS, HTTP, JSON, and SQLite plumbing; a manual memory model
(arenas, ref-counted command contexts); and a toolchain (CMake, sanitizers,
`clang-tidy`, Doxygen, custom consistency checks C1–C18). A feature or a fix
touched many files and required a sanitizer run to trust. The bot's actual
demands — moderate a group, talk to the Bot API, persist a little state — are
well served by a high-level async stack.

After an independent review of the Python port found it healthy (252 tests, 87 %
coverage, `ruff` and `mypy --strict` clean), **the owner accepted the Python
stack.** This ADR records that decision.

## Decision

Rewrite the bot in **Python 3.12**, using **`asyncio`** for concurrency,
**[aiogram 3](https://docs.aiogram.dev)** as the Telegram framework,
**`aiosqlite`** for storage, and **`uv`** for the toolchain. Keep the behaviour
that the original specification defined: the command set, the permission model,
the automod pipeline, the captcha flow, the expiry sweeper, the rate limits, and
the data model.

The C implementation is not deleted; it remains reachable at the
`c-implementation-final` tag.

## Consequences

### Gained

- Far less code and ceremony: no manual memory management, no hand-written TLS,
  HTTP, or JSON.
- A mature async framework (aiogram) with an update dispatcher, middlewares, and
  typed Bot API models, plus `pytest`, `ruff`, and `mypy --strict` for
  confidence.
- Faster iteration: a fix is one file and a test run, not a rebuild under a
  sanitizer.

### Lost / changed

- **No `io_uring`.** The event loop is Python's selector-based `asyncio` loop.
  Per-syscall batching and kernel-level async I/O are gone.
- **No sanitizers.** Memory-safety tooling (ASan/TSan/UBSan) no longer applies.
  Its role is partly taken by the debug allocator and `PYTHONASYNCIODEBUG`
  (`-X dev`), run in the soak smoke mode, and by `ruff`'s `S` (bandit) rules and
  `pip-audit`.
- **The GIL.** Only one thread executes Python bytecode at a time. Blocking I/O
  inside `aiosqlite` and the TLS stack releases the GIL, which keeps the loop
  responsive under load, but CPU-bound Python work is serialised.
- **A looser latency budget.** The C loop's few-millisecond stall budget is
  replaced by a 100 ms event-loop-lag budget and a 50 ms p99 handler-latency
  budget, documented in [ADR 0002](0002-python-loop-lag-budget.md).
- **A new teardown hazard.** A pydantic/aiogram interaction (a cached `Update`
  surviving to interpreter finalization) segfaults under the debug allocator; the
  bot clears that cache at shutdown. See the fix in `main._clear_aiogram_caches`.

## Alternatives

- **Keep the C implementation.** Rejected: the maintenance cost exceeded the
  value, and the owner accepted the Python stack after the review. The C code
  stays available at the `c-implementation-final` tag.
- **A different Python framework (python-telegram-bot, raw `aiohttp`).**
  Rejected: aiogram 3's dispatcher, middlewares, and typed models fit the command
  registry and middleware design directly.
- **Go or Rust.** Rejected: would repeat the port cost while discarding the
  Python tooling already in place; the owner's choice was Python.
