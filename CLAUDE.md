# nacho-moderator

An io_uring-based Telegram admin/moderator bot written in C11.

## Specs are binding

This project is governed by four specification documents. **Read all of them before writing any
code:**

1. **[`docs/SPEC.md`](docs/SPEC.md)** — the base spec: goal, architecture, io_uring rules,
   features, data model, config, security, testing, and the M1–M7 milestones.
2. **[`docs/SPEC-ADDENDUM-TOOLCHAIN.md`](docs/SPEC-ADDENDUM-TOOLCHAIN.md)** — the toolchain,
   build, testing, and CI addendum.
3. **[`docs/SPEC-ADDENDUM-DOCS.md`](docs/SPEC-ADDENDUM-DOCS.md)** — the documentation addendum:
   Doxygen/README/prose docs, comment conventions, docs CI jobs, and the documentation
   Definition of Done (D9).
4. **[`docs/SPEC-ADDENDUM-SSOT.md`](docs/SPEC-ADDENDUM-SSOT.md)** — the single-source-of-truth
   addendum: X-macro `.def` tables, the introspection CLI, generated doc blocks, and the
   `check_consistency.py` checks C1–C18.

**Precedence:** an addendum wins wherever it conflicts with the base spec. The toolchain addendum
explicitly supersedes:
- the "Build" row in base spec section 3,
- the `/tests` entry in base spec section 8,
- base spec section 11 acceptance item 2 (**Valgrind is NOT usable with io_uring — use
  sanitizers instead**).

The toolchain addendum's test strategy (§A5) and CI pipeline (§A6) are the authoritative expansion
of base spec section 11; its milestone **M0** (toolchain skeleton) comes *before* base spec **M1**.
The docs addendum's D9 **adds to** the toolchain Definition of Done (A9) — every milestone/PR must
satisfy both checklists, and the documentation skeleton (D11) is also created in **M0**.
The SSOT addendum (S10) also lands in **M0** (tables, dump flags, `docgen.py`, checks C1–C5), and
it **supersedes** the informal `commands.def`/`config.def` mentions in the docs addendum (D6, D8) —
where they conflict, S1–S10 win.

These rules are not suggestions. In particular, the agent MUST:

1. **Build the M0 toolchain skeleton first** (CMake presets, sanitizers, `.clang-format`/
   `.clang-tidy`, vendored `cmocka`, one trivial passing test, CI workflow) *before* writing any
   feature code (A10), then implement base-spec milestones M1–M7 in order (§12).
2. **Write unit tests in the same commit as the module** — test-first for parsers and the rate
   limiter (A10). Every new module needs a test; parsers/untrusted-input code needs a fuzz target.
3. **Never disable a sanitizer or a warning to make something pass.** Fix the root cause or ask.
4. **Use the injectable interfaces** (`clock_ops`, `net_ops`, `db_ops`, `tg_api_ops`, injectable
   random) for anything involving time, randomness, network, or DB — never the real ones (A3, A10).
5. **Never call `time()`/`clock_gettime()` directly in logic modules** (A3).
6. **Keep the main thread free of blocking syscalls** (`read`, `connect`, `poll`, `futex` wait,
   SQLite/file I/O) — validated by `scripts/check_blocking.sh` (A4, A9; base spec §11.7).
7. **Meet the Definition of Done** for every milestone/PR (A9): warning-free on GCC **and** Clang
   with `-Werror`, `clang-format`/`clang-tidy`/`cppcheck` clean, `ctest` green under `asan`
   (and `tsan` if threads touched), coverage thresholds met, docs updated, Conventional Commits.
8. **Write Doxygen comments in the same commit as the code** (D5, D11): a function without
   documentation is an incomplete function. Every new module needs an `@file` header and an
   `@ingroup`; `docs-check` must pass with zero warnings (D9, D10).
9. **Keep a single source of truth for commands and config keys** — define each once in its
   `.def` X-macro table (`src/commands.def`, `src/config.def`; plus `locks.def`, `api_methods.def`,
   `errors.def`, `metrics.def`), and derive/verify everything else (enums, dispatch, `/help`,
   `setMyCommands`, docs, man pages, sample config) from it via the introspection CLI and
   `scripts/docgen.py` / `scripts/check_consistency.py` (S1–S10, which supersede D6/D8). Never add
   a command/key/lock/API method/error class/metric anywhere but its `.def` file; never hand-edit
   between `BEGIN GENERATED`/`END GENERATED` markers; update README, `docs/`, and CHANGELOG
   `Unreleased` in the same PR (D9).
10. **Never weaken, skip, or delete a consistency check** (C1–C18) or a test to make something
    pass — fix the root cause, or add an ADR and ask (S10).
11. **After each milestone, print the exact commands** to build, run all tests, run the fuzzers,
    produce the coverage report (A10; base spec §13), run `docs-check` (D11), and run
    `scripts/check_consistency.py` (S7).

Base-spec hard rules that remain in force (never deviate without asking — base spec §13):
all network I/O goes through `liburing` in a single-threaded loop with **no epoll/poll fallback**;
TLS via OpenSSL **memory BIOs** only (never blocking `SSL_read`/`SSL_write` on the socket);
SQLite touched **only** from the DB worker thread, signalled to the loop via `eventfd`;
prepared statements only; HTML-escape user input; no dependencies beyond liburing, OpenSSL,
SQLite3, yyjson (or cJSON), and optionally PCRE2.

Do **not** use Valgrind on anything that touches the io_uring ring.

## Layout (target)

- Base spec §8 lists the `/src` module layout; addendum A2 adds the canonical repository layout
  (`/cmake`, `/include`, `/tests/{unit,integration,fuzz,mock,fixtures}`, `/scripts`, `/packaging`,
  `/third_party`).

## Conventions

- Language: C11. Naming: `snake_case`, `module_verb_noun()`, types `_t`, macros/enums `UPPER_SNAKE`.
- Every fallible function returns `int` (0 = ok, negative errno) or a tagged result; no silent
  failures. No `strcpy/strcat/sprintf/gets/atoi`.
- Document allocation/free ownership in header comments; per-update arena allocator (A7, §9).
- Comment every io_uring state transition (base spec §13).
