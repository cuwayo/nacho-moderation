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

### Added (M3)

- M3: update parsing (`src/tg_types.c`) — bounded Update/Message/User/Chat structs
  for `message`, `edited_message`, `callback_query`, `chat_member` and
  `my_chat_member`, tolerating missing/null/extra fields and huge int64 ids.
- M3: the SQLite worker thread (`src/db.c`): an MPSC job queue, prepared upserts,
  a `schema_version`-driven migration creating the SPEC §6 schema, WAL pragmas, and
  an eventfd handoff read through io_uring (`OP_EVENTFD`) with an injectable
  `db_ops_t` seam.
- M3: a TTL hash map (`src/cache.c`) with an injectable clock and bounded eviction.
- M3: the permission model and per-chat administrator cache (`src/perms.c`):
  role/right checks, sudo bypass, target guards, and a live `getChatAdministrators`
  fetch cached for 10 minutes and invalidated on `chat_member` and `/reload`.
- M3: the command router (`src/router.c`): `/command[@bot]` parsing, target
  resolution (reply → numeric id), permission gating, and dispatch to the command
  handler, with `users`/`chats` upserts on every message.
- M3: a real `/help` reply rendered from the command table, plus the
  `setMyCommands` payload registered at startup (`src/commands.c`, `src/cmd_help.c`).
- M3: `poll_batch_t` now carries the raw reply body so the poller can hand updates
  to the router, and `config_get_i64_list()` parses `sudo_ids`.
- M3 API methods: `getChatAdministrators`, `sendMessage`, `setMyCommands` (each
  with a mock-server handler and a docs row, C9).
- M3 error class `PERM` for permission denials.
- M3 tests: `test_tg_types`, `test_cache`, `test_db` (real temp SQLite), `test_perms`
  (the A5.1 matrix), `test_router`, an updated `test_cmd_help`, and the
  `fuzz_update_json` fuzz target; plus the `integration-router` end-to-end test.

### Added (M4)

- M4: member-moderation commands `src/cmd_mod.c` — `/ban`, `/unban`, `/kick`,
  `/mute`, `/unmute` — with optional `[duration] [reason]`, the target guard
  (never the owner, another admin, or the bot), and an `actions` row recorded per
  action (SPEC section 5.2).
- M4: message-management commands `src/cmd_msg.c` — `/del` (two `deleteMessage`
  calls), `/pin` (silent), `/unpin`, and `/purge` (`deleteMessages` in batches of
  100, up to 1000 ids) (SPEC section 5.2).
- M4: a reference-counted command invocation (`src/cmd_ctx.c`, `include/tg/cmd_ctx.h`)
  so a handler may start an asynchronous chain (`/kick` = ban → unban; `/purge`
  fan-out) and the context outlives the handler call, freed by the last callback
  (SPEC section 9).
- M4: an injectable wall clock (`wall_clock_ops_t` in `src/util.c`) for absolute
  `until_date` / `expires_at` timestamps, kept separate from the monotonic clock
  used for timers (SPEC-ADDENDUM-TOOLCHAIN A3).
- M4: the expiring-actions sweeper (`src/expiry.c`): a 30 s timer asks the DB
  worker for due `actions` rows and lifts them (`unbanChatMember` / restored
  `restrictChatMember` permissions), marking each row lifted; it survives restarts
  because `expires_at` is wall-clock (SPEC section 5.6).
- M4: the router now resolves the target from a reply, a numeric id, or an
  `@username` (via the `users` cache) and derives the caller/target/bot roles from a
  single `getChatAdministrators` fetch (`src/router.c`).
- M4: DB jobs for `actions` insert, the expiry sweep, the lift update, and a
  username lookup (`src/db.c`, `include/tg/db.h`).
- M4 API methods: `banChatMember`, `unbanChatMember`, `restrictChatMember`,
  `deleteMessage`, `deleteMessages`, `pinChatMessage`, `unpinChatMessage` (each
  with a mock-server handler and a docs row, C9).
- M4 tests: `test_cmd_util`, `test_cmd_mod`, `test_cmd_msg`, `test_expiry`, and the
  DB action/expiry/lookup cases in `test_db`; a `fuzz_moderation_args` fuzz target;
  and the `integration-moderation` end-to-end test with `ban_reply`,
  `ban_nonadmin`, `ban_username`, and `kick_reply` scenarios.

### Fixed

- M1/M2 TLS: a request queued before the handshake completed (the API pool queues
  it while the connection is still `CONNECTING`) was encrypted with `SSL_write()`
  before the session existed. OpenSSL refused the bytes and retained nothing, so
  the request was silently dropped and the loop then blocked forever in
  `io_uring_wait_cqe` on every HTTPS request. `conn_send_request()` now buffers
  the plaintext and `conn_pump()` encrypts and flushes it once the handshake
  completes (`src/net_conn.c`); regression test
  `test_tls_request_queued_before_handshake` added.
- M3 router: commands in a **private chat** failed with `permission resolution
  failed: 104` and were dropped. The router always called `getChatAdministrators`,
  which Telegram rejects for private chats (`ok=false` → `ERR_TG_API`). It now
  resolves the caller locally for private chats (plain member unless sudo) and only
  fetches the admin list in groups; a failed lookup now replies that permissions
  could not be verified instead of dropping the command silently
  (`src/router.c`); regression test `test_private_chat_help_replies_without_admin_fetch`
  added.
- M4 `/purge` deleted only the ids it had observed in an in-memory ring, so after a
  restart (or for messages sent before the bot started) it removed just the replied-to
  message and the command, appearing not to work. It now deletes the whole
  sequential id range from the replied-to message to the command, independent of what
  the bot observed (`src/cmd_msg.c`); the now-unused router ring was removed, and
  `test_purge_covers_reply_to_command_range` / `test_purge_caps_at_max_ids` plus the
  `purge_range` integration scenario were added.
- M4 expiry: a **permanent** action was stored with `expires_at = 0`, which the sweep
  query (`expires_at IS NOT NULL AND expires_at <= now`) treated as already due. Every
  sweep therefore "lifted" every permanent action — log spam for `/purge`/`/del`, and,
  worse, a permanent `/ban` was silently undone within 30 s. The insert now stores
  `NULL` for a permanent action and the query additionally requires `expires_at > 0`
  (`src/db.c`); regression test `test_permanent_action_is_never_swept` added.
- M4 `@username` targets were unresolvable unless the user had spoken since the bot
  started, and even then the admin list's own usernames were discarded. The router now
  resolves `@username` against the administrator list it already fetched (in memory)
  and seeds the `users` cache from it, and also upserts the replied-to message's
  sender; the "unknown username" reply now suggests replying or using a numeric id
  (`src/router.c`, `src/perms.c`); `test_admin_fetch_captures_username` and the
  `unban_admin_username` integration scenario added.

[Unreleased]: https://github.com/cuwayo/nacho-moderation/commits/main
