# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- M0 toolchain skeleton: CMake presets (`debug`, `asan`, `tsan`, `coverage`,
  `fuzz`, `release`), sanitizer/warning/hardening modules, vendored `cmocka` and
  `yyjson`, `clang-format`/`clang-tidy`/`cppcheck` configuration, CI workflow, and
  one trivial passing test.
- Single source of truth tables (`src/commands.def`, `src/config.def`, and the
  `locks`, `api_methods`, `errors`, `metrics` tables) with X-macro generated C.
- Introspection CLI: `--dump-commands-json`, `--dump-config-json`,
  `--dump-tables-json`, `--dump-config-example`, `--check-config`, `--help`.
- `scripts/docgen.py` (generated doc blocks) and `scripts/check_consistency.py`
  (checks C1–C5), with their own unit tests.
- Documentation skeleton: Doxygen config and groups, prose docs, ADRs, man page
  sources, README, CONTRIBUTING, SECURITY, and this changelog.
- M1: single-threaded io_uring event loop with SQE batching, CQE dispatch by op
  type, and feature probing with fallback (`src/loop.c`).
- M1: TCP connection state machine (`src/net_conn.c`) and an OpenSSL memory-BIO
  TLS adapter driven by the loop (`src/tls.c`).
- M1: incremental HTTP/1.1 response parser (Content-Length, chunked, keep-alive)
  and a request builder (`src/http.c`).
- M1: worker-thread DNS resolution with an `eventfd` handoff (`src/resolver.c`)
  and a minimal Bot API client performing `getMe` (`src/tg_api.c`).
- M1: utilities — per-update arena allocator, string builder, duration parser,
  HTML escaping, injectable clock, and a structured logger (`src/util.c`).
- M1: table-driven configuration loader with CLI > env > file > default
  precedence (`src/config.c`).
- Tests: 17 unit test binaries plus the `getMe` integration test against the mock
  Telegram server; fuzz targets for the HTTP parser, HTML escaping, and the
  duration parser; a `fake_ring` test double for the loop interfaces.
- CI: `docs-lint`, `docs-build`, and `docs-consistency` jobs.

### Added (M2)

- M2: reusable keep-alive connections — `conn_keep_alive()` / `conn_reuse_begin()`
  reissue a request on the same socket and TLS session (`src/net_conn.c`), plus an
  HTTP/1.1 POST request builder with a JSON body (`src/http.c`).
- M2: outbound token-bucket rate limiter — global 25 req/s, per-chat 15 req/min
  for messages and 1 req/s for bulk, with 429 `retry_after` pausing and the
  1/2/4/8/16/30 s backoff schedule, driven by the injectable clock
  (`src/rate_limit.c`).
- M2: retry policy (`src/api_retry.c`) keyed on the idempotency flag in
  `src/api_methods.def`, and a C view of that table (`src/api_methods.c`).
- M2: keep-alive API connection pool with a request queue, rate-limit gating,
  retries and a single dispatch timer (`src/api_pool.c`), and the API client
  facade with method wrappers and response parsing (`src/tg_api.c`). The Bot API
  host is resolved once at startup on the worker-thread resolver
  (`src/resolver.c`), reused for every connection.
- M2: `getUpdates` long-poll loop tracking `offset = last_update_id + 1` with an
  explicit `allowed_updates` set (`src/polling.c`); the default run now polls
  until SIGINT/SIGTERM.
- M2: runtime counters generated from `src/metrics.def`, dumped on SIGUSR1
  (`src/metrics.c`).
- M2 config keys: `api.request_timeout_ms` and `polling.timeout`.
- M2 tests: rate limiter, retry policy, API method table, connection reuse
  (plain + TLS), POST builder, response parsing, and the polling loop, plus a
  `fuzz_api_response` fuzz target and a polling integration scenario.
- M2 consistency check C9: every `api_methods.def` method has a mock-server
  handler and a `docs/telegram-api-notes.md` row.

[Unreleased]: https://example.invalid/nacho-moderator/commits/main
