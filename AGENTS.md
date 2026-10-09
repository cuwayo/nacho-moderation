# AGENTS.md

This project is an io_uring-based Telegram admin/moderator bot written in C11.

**Before writing any code, read and follow all four binding specs:**

- [`docs/SPEC.md`](docs/SPEC.md) — the base spec (architecture, io_uring rules, features, data
  model, security, milestones M1–M7).
- [`docs/SPEC-ADDENDUM-TOOLCHAIN.md`](docs/SPEC-ADDENDUM-TOOLCHAIN.md) — toolchain, build,
  testing, and CI addendum. **It wins on any conflict** and supersedes the "Build" row in section 3,
  the `/tests` entry in section 8, and section 11 acceptance item 2 of the base spec (Valgrind is
  not usable with io_uring — use sanitizers). It adds milestone **M0**, which comes before M1.
- [`docs/SPEC-ADDENDUM-DOCS.md`](docs/SPEC-ADDENDUM-DOCS.md) — documentation addendum (Doxygen,
  README, prose docs, comment conventions, docs CI jobs). Its documentation Definition of Done (D9)
  **adds to** the toolchain DoD (A9), and its skeleton (D11) is also created in M0.
- [`docs/SPEC-ADDENDUM-SSOT.md`](docs/SPEC-ADDENDUM-SSOT.md) — single-source-of-truth addendum
  (X-macro `.def` tables, introspection CLI, generated doc blocks, `check_consistency.py` C1–C18).
  It **supersedes** the informal `commands.def`/`config.def` mentions in the docs addendum (D6, D8)
  and its core (S10) also lands in M0.

The full, authoritative agent instructions (M0 skeleton first, test-with-code, no disabled
warnings/sanitizers, injectable clock/random/net/db, no blocking syscalls on the main thread,
Definition of Done, Conventional Commits) are in **[`CLAUDE.md`](CLAUDE.md)** — follow them.
