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

### Added (M5)

- M5: warning commands `src/cmd_warn.c` — `/warn` (records a warning and, at the
  per-chat limit, applies the configured action and clears the warnings),
  `/unwarn`, `/warns` and `/resetwarns` (SPEC section 5.2). `/warns` is
  self-service: with no target any member sees their own warnings, while naming
  another user is admin-only.
- M5: settings, welcome/rules and reload commands `src/cmd_settings.c` —
  `/setwarnlimit`, `/setwarnaction`, `/setlog` (owner-only), `/reload`,
  `/setwelcome`, `/welcome on|off`, `/setrules`, `/rules` (readable by any member)
  and the `/settings` inline-keyboard panel (SPEC sections 5.2, 5.4, 5.5).
- M5: join-time welcome delivery — the bot greets new members in supergroups
  (`chat_member`) and basic groups (`new_chat_members`), rendering the
  `{first}`/`{mention}`/`{chat}`/`{count}` placeholders and fetching the member
  count with `getChatMemberCount` (SPEC section 5.4).
- M5: the moderation log — every moderation action also posts to the chat's
  configured log channel when `setlog` is set (SPEC section 5.5).
- M5: a per-chat settings service (`src/settings.c`, `include/tg/settings.h`) with
  a TTL cache in front of the DB worker, mirroring the administrator cache, plus
  the `chats` policy columns it reads and writes (SPEC section 4.4).
- M5: DB worker jobs for warns (add/list/remove-last/reset) and settings
  (masked get/set) with prepared statements (`src/db.c`, `include/tg/db.h`); the
  `chats` schema already covered M5, so no migration was needed.
- M5: `cmd_ctx` gained a settings snapshot, a `db_submit` hook (retains the
  invocation across a DB job), admin/settings cache-invalidator hooks, and
  `cmd_ctx_request_chat` for rate-limiting an outbound call against an explicit
  chat (the moderation log).
- M5: flag-driven command parsing — `router_parse_command_ex` consumes a leading
  target token only for commands that take one, so `/setwarnlimit 5` and
  `/setlog -100…` keep their numeric argument.
- M5: inline-keyboard callbacks — the router parses `callback_query.message` and
  dispatches a settings-panel press, enforcing the SPEC section 10 user-ownership
  check before toggling a setting.
- M5 API methods: `getChatMemberCount`, `answerCallbackQuery`, `editMessageText`
  (each with a mock-server handler and a docs row, C9).
- M5 tests: `test_cmd_warn`, `test_cmd_settings`, `test_settings`, extended
  `test_db` (warn/settings jobs), `test_cmd_mod` (log post), `test_router`
  (flag-driven parsing) and `test_tg_types` (callback message); a
  `fuzz_callback_data` fuzz target; and the `integration-m5` end-to-end test with
  `warn_limit_action`, `welcome_join`, `welcome_join_part2` and `settings_panel`
  scenarios.

### Added (M6)

- M6: automated moderation (`src/automod.c`, `include/tg/automod.h`) running on
  every group message before command dispatch, in the SPEC section 5.3 order
  **locks -> blacklist -> antiflood**. Administrators, the owner and sudo users
  are skipped; deletions, warnings and mutes are issued through the API/DB seams
  and recorded as `actions` rows with the moderation log (SPEC section 5.5).
- M6: **locks** — a per-chat bitmask (`chats.locks`) generated from `src/locks.def`
  (`links`, `media`, `stickers`, `forwards`, `polls`, `invites`, `bots`,
  `commands`); a locked message is deleted and logged. The message classifier
  (`automod_locked_content`) is pure and unit-tested.
- M6: **blacklist** — word and `/regex/` patterns (`blacklist` table) matched
  case-insensitively over the text or media caption; a match deletes the message,
  warns the sender and applies the configured warn action at the limit, reusing
  the `/warn` auto-action. Regexes are compiled with PCRE2 under match/depth
  limits so a hostile pattern cannot stall the loop (SPEC section 10).
- M6: **antiflood** — an in-memory sliding window per `(chat_id, user_id)` (SPEC
  section 5.3): exceeding the threshold mutes the sender (temporary restriction
  the expiry sweeper lifts) and deletes the burst messages. The window arithmetic
  (`automod_flood_track`) is pure and driven by the injected clock.
- M6: **join captcha** (`src/captcha.c`, `include/tg/captcha.h`, SPEC section 5.4)
  — a new member is immediately restricted, sent an inline "I'm human" button and
  recorded in `pending_captcha` with a deadline; a correct-user press lifts the
  restriction, deletes the prompt and delivers the welcome, a wrong-user press is
  rejected (SPEC section 10), and a periodic sweep kicks members whose deadline
  passed (ban then unban). The callback parser (`captcha_parse_callback`) is pure
  and fuzz-tested; the nonce comes from a new injectable random source
  (`random_ops_t`, seeded once at startup) so tests are deterministic
  (SPEC-ADDENDUM-TOOLCHAIN A3).
- M6: automod configuration commands `src/cmd_automod.c` — `/lock`, `/unlock`
  (multiple lock types, comma/space separated), `/blacklist add|del|list`,
  `/antiflood <messages> <seconds>|off` and `/captcha on|off` (SPEC section 5.2).
- M6: the message parser (`src/tg_types.c`) now captures entities, captions,
  media/sticker/animation/forward/poll presence flags and the content type, so
  locks and the blacklist can classify a message without the raw JSON.
- M6: DB worker jobs for the blacklist (`DB_JOB_BLACKLIST_*`) and pending captcha
  (`DB_JOB_CAPTCHA_*`), the new `chats` automod columns (captcha, antiflood,
  locks) in the settings snapshot, and an injectable random source in `util`.
- M6 tests: `test_automod` (lock classifier, blacklist matcher, flood window),
  `test_captcha` (parser, callback verification, timeout sweep) and
  `test_cmd_automod` (command argument parsing), extended `test_db` (blacklist,
  captcha and the automod settings columns); a `fuzz_captcha_callback` fuzz
  target; and the `integration-m6` end-to-end test with `automod_locks`,
  `automod_antiflood`, `captcha_join`, `captcha_join_part2` and
  `captcha_wrong_user` scenarios.

### Added (M7)

- M7: **webhook mode** (`src/webhook.c`, `include/tg/webhook.h`, SPEC section 4.1)
  — an inbound plain-HTTP server for use behind a TLS-terminating reverse proxy.
  The listening socket is bound once at startup; accepts (`io_uring_prep_accept`),
  receives and sends are all io_uring operations. Each request is parsed by a new
  incremental request parser (`http_req_t` in `src/http.c`), checked against
  `webhook.secret` with a constant-time compare (403 on mismatch, 405 on a non-POST
  method), and routed as one Update. `mode = "webhook"` selects it over the
  default `polling` at startup (`src/main.c`).
- M7: **signal handling via `signalfd` read on the ring** (`src/signals.c`,
  `include/tg/signals.h`, SPEC section 8). `SIGINT`/`SIGTERM`/`SIGUSR1` are blocked
  and delivered through a signalfd whose read is a ring operation, so an arriving
  signal produces a CQE and wakes the loop (an async handler only setting a flag
  does not, because `io_uring_wait_cqe` restarts itself on EINTR).
- M7: **graceful shutdown** (SPEC section 9) — on a termination signal the bot
  stops its update source, then drains in-flight Bot API requests and DB jobs
  bounded by 10 s before flushing and exiting 0. A new bounded loop wait
  (`loop_run_once_timeout`, `io_uring_wait_cqe_timeout`) keeps a long timer from
  blocking past the drain budget, and `api_pool_outstanding` /
  `api_pool_outstanding_class` report what is left.
- M7: **packaging** — a hardened systemd unit (`packaging/nachomoderator.service`)
  with the io_uring-aware `SystemCallFilter` from SPEC-ADDENDUM-TOOLCHAIN A8, and a
  multi-stage `packaging/Dockerfile` with the seccomp caveat documented.
- M7: **SSOT consistency checks C6–C18** (`scripts/check_consistency.py`): TOML
  snippets validate, README `title="ci"` snippets parse with known presets,
  `--help` matches the golden file and `docs/building.md`, man pages list every
  command and config key, `setMyCommands` constraints hold, Doxygen `@ingroup`/
  `@file` are present, no bare `TODO`/`FIXME`, `CHANGELOG`/`.def` PR-context checks,
  metrics and error classes are documented, relative doc links resolve, and
  forbidden terminology is absent. Each has a passing and a failing fixture in
  `tests/scripts/test_check_consistency.py`.
- M7: `new http_req_t` request parser and `http_response_build()` (`src/http.c`),
  `net_ops_t` `sock_bind`/`sock_listen`, `loop_ops_t` `prep_accept`/`prep_read`/
  `wait_cqe_timeout`, and a `metrics-table` generated doc block in
  `docs/architecture.md`.
- M7 tests: `test_http_request`, `test_webhook` (over the fake ring) and
  `test_signals`, the `fuzz_http_request` fuzz target, and the
  `integration-webhook` end-to-end test (valid update routed, wrong secret 403,
  GET 405, SIGTERM exits 0). CI gains a `docs-smoke` job that runs the README
  quick-start verbatim.

### Fixed

- M7 shutdown: a termination signal did not stop the bot. `io_uring_wait_cqe`
  restarts itself on `EINTR`, so an asynchronous handler that only set a flag never
  woke the loop, and the process hung until the 30 s long-poll returned. Signals
  are now delivered through a signalfd read on the ring (`src/signals.c`).
- M7 shutdown: the graceful drain could block for far longer than its 10 s budget
  (or forever) because it used the unbounded `loop_run_once`. The drain now uses
  `loop_run_once_timeout` with a short slice, so a pending sweep timer or a
  missed eventfd wakeup cannot stall shutdown.
- M7 webhook: an accepted connection that arrived after `webhook_stop()` was
  leaked (there is no re-arm to service it); it is now closed. A failed
  `prep_recv`/`prep_send` (SQ ring momentarily full) likewise released the
  connection slot instead of leaving it stuck, and the shutdown drain now also
  waits for in-flight webhook responses (`src/webhook.c`, `src/main.c`).
- M7 HTTP: the response and inbound-request parsers deferred a malformed-line
  error to the *next* byte (or forever if no byte followed), because the line
  handler returned success while the state was already `HR_ERROR`/`HRQ_ERROR`.
  Both now report `-EPROTO` on the byte that completes the bad line
  (`src/http.c`); regression tests added.
- M7 main: a failure in `tg_api_create()` returned without destroying the signal
  watcher (a leak), now fixed (`src/main.c`).

- M6: `foreach_token`'s unrecognized-token pointer in `/lock`/`/unlock` pointed
  into a stack buffer that did not outlive the visitor, so the "Unknown lock
  type" reply read freed stack memory. The token is now copied into the parse
  accumulator (`src/cmd_automod.c`); caught by the ASan build.

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
