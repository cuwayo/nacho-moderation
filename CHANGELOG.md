# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased] (0.2.0 candidate)

### Security

- **Redaction now covers every log path.** Log redaction moved from a record
  filter to a custom `logging.Formatter` that rewrites the final rendered line,
  so the bot token and webhook secret are masked when they are assembled at
  render time (`.../bot<token>/getMe` with a `%s` argument), in tracebacks and
  `exc_info`/`stack_info` text, and in the `aiogram`, `aiohttp`, and `asyncio`
  loggers, which propagate to the root handler. Secrets are matched by exact
  registered value (token, webhook secret, URL-quoted form, and the part after a
  token's `:`) and by shape (`bot<digits>:<secret>` and bare `<digits>:<secret>`).
  `Settings.__repr__`/`__str__` mask the token and secret, and a config
  validation error no longer echoes the rejected value (which previously printed
  the token to stderr).
- **A secret-bearing config file must be private.** The bot now refuses to start
  when the config file holds a `bot_token` or `webhook.secret` and its mode has
  any group/other bit set (`mode & 0o077`), and tells the user to `chmod 600` it.
  The check resolves symlinks first (the target's mode is inspected). An
  environment-only configuration is unaffected. `--allow-insecure-config` (env
  `ALLOW_INSECURE_CONFIG=1`) overrides the check for containers and CI.

### Fixed

- **Gen-2 GC pauses stalled the event loop under sustained load.** At 1000
  updates/s the soak measured isolated >100 ms event-loop-lag spikes after
  warm-up, at ~10 s intervals. Instrumenting `gc.callbacks` attributed them to
  gen-2 collections walking the large, stable long-lived object graph (the
  aiogram bot, dispatcher, services, caches, and every pydantic type); disabling
  gen-2 GC after startup dropped the steady-state lag max from ~174 ms to ~76 ms
  with no post-warm-up event over 100 ms. `run_bot` now calls
  `gc.freeze()` once, after startup, moving that graph to CPython's permanent
  generation so the collector never scans it again; per-update objects stay
  collectable. See [ADR 0002](docs/adr/0002-python-loop-lag-budget.md).
- **`/promote` no longer loses a promotion when the title call fails.** Bot API
  9.3 moved `setChatAdministratorCustomTitle` out of `promoteChatMember`, so the
  command makes two calls. If the promotion succeeded but the title call raised,
  the old code replied "Telegram rejected that change" and recorded nothing,
  although the user *was* promoted. The two calls are now handled separately: the
  promotion is recorded, and the reply is "Promoted, but the title could not be
  set." The success reply never claims a title Telegram refused.
- **`test_config_example_validates` failed on a fresh checkout.** Git checks the
  tracked `config/config.example.toml` out as 0644 (umask 022), and the P2
  permission check refuses a group/other-readable secret, so the test failed in
  CI and on every clone. It now copies the example into `tmp_path`, `chmod 600`,
  and validates the copy — the same mode a real deployment uses — without
  weakening the permission check or reaching for `--allow-insecure-config`. A
  regression test materialises a fresh checkout under `umask 022` and asserts
  the copy validates.
- Logging a single mapping argument (`log.info("x %s", {"a": 1})`) printed `x a`
  instead of `x {'a': 1}`: the old redacting filter iterated the mapping's keys.
  Redaction no longer mutates `record.msg`/`record.args`, so mapping and tuple
  arguments are formatted correctly.
- `webhook.listen = "[::1]:8080"` produced the host `[::1]`, which `aiohttp`
  cannot bind. The address is now parsed by a tested `parse_listen()` that
  returns the unbracketed host and validates the port at config-load time, so
  `--check-config` catches a malformed address. A bare port, an empty host, an
  unbracketed IPv6 address, and a port outside `1-65535` are rejected.
- `Database.connect` leaked the aiosqlite connection (and its worker thread) if
  applying the pragmas or the migration failed after the connection opened. The
  connection is now closed before the error propagates.
- An intermittent `PytestUnhandledThreadExceptionWarning` under `pytest-randomly`
  (an aiosqlite worker thread calling back into a closed per-test event loop).
  The test suite now runs all async tests on one session-scoped loop, matching
  production's single long-lived loop.

### Changed

- **The soak test now measures real load.** The mock served one update per
  `getUpdates` (`SOAK_BATCH = 1`), so a "1000/s" run actually carried ~3 % of its
  stated load (throughput was capped at the ~26 updates/s poll round-trip) while
  the gate passed against a fixed minimum of 1. The mock now serves up to 100
  updates per reply (Telegram's real default), configurable with `--soak-batch`,
  and its pending-update queue is bounded (oldest dropped, with a warning, past
  5 s of backlog). The gate derives its minimum handler-sample count from the run
  itself (`>= 0.8 * rate * measured_seconds`), prints the requested rate, achieved
  rate (`latency_count / seconds`), and handler sample count, and fails when the
  achieved rate is below 80 % of the requested rate. Loop lag is split into a
  warm-up window (first 60 s; only a startup hang over 1000 ms fails) and steady
  state (p99 <= 100 ms, max <= 250 ms), and the ten largest lag events are logged
  with their elapsed time. The metrics line carries `uptime_seconds` and the
  windowed lag numbers; the histogram's open-ended bucket now reports the observed
  maximum instead of `inf`. Timing budgets run with production flags: `smoke`
  and `nightly` enforce every budget, and a new `hygiene` mode runs under
  `PYTHONASYNCIODEBUG`/`-X dev` and checks hygiene only. Measured numbers are in
  [ADR 0002](docs/adr/0002-python-loop-lag-budget.md).
- **Python toolchain hardening.** The lint set gains `D` (Google docstrings),
  `ANN`, `PT`, `PL`, `PERF`, `TRY`, `ARG`, `ERA`, `T20`, `LOG`, `G`, `PTH`, and
  `FBT` (every `ignore` justified in `pyproject.toml`); `mypy --strict` now
  covers `tests` and `scripts` as well as `src`. Tests run under
  `pytest-asyncio` (auto), `pytest-randomly`, and `pytest-timeout` (30 s), and CI
  runs the suite with `PYTHONASYNCIODEBUG=1 -X dev -W error`. Coverage uses
  `--cov-branch` with a **global floor of 90%** and per-module floors
  (`transport/*`, `services/perms.py`, `services/automod.py`, `commands/args.py`,
  `config.py`, `logging_setup.py` ≥ 95%; `handlers.py`, `main.py` ≥ 85%) enforced
  by `scripts/check_coverage.py`. Added `pip-audit`, `bandit` (ruff `S`),
  `detect-secrets` (with a committed baseline), Dependabot, a pre-commit config,
  a `pydoc` API-docs job, a hardened Dockerfile (non-root, digest-pinned,
  `HEALTHCHECK`) and systemd unit, and CI jobs for lint, typecheck, test,
  coverage, security, docs-lint, docs-smoke, docs-consistency, docs-build, and
  soak. GitHub Actions are pinned by commit SHA.

- **Project spec restored for Python.** `docs/SPEC.md` documents the current
  stack (goals, non-goals, architecture, update sources, the 31-command table,
  automod, captcha, expiry, data model, config, security, acceptance criteria),
  keeping the still-valid behaviour from the original spec (rate limits 25/s
  global, 15/min per chat, 1/s bulk, 429 `retry_after`, backoff 1/2/4/8/16/30 s,
  captcha `cap:<chat>:<user>:<nonce>` ≤ 64 bytes, temp actions surviving
  restart). ADR [0001](docs/adr/0001-python-asyncio-aiogram.md) records leaving
  C/`liburing` and ADR [0002](docs/adr/0002-python-loop-lag-budget.md) the Python
  latency budgets.

- **Complete rewrite in Python 3.12 using `uv` and `aiogram 3`.** The bot is now
  a single-process asyncio application. The previous C11/`liburing` implementation
  (milestones M0–M7) is preserved in git history and superseded by this port.

### Added

- **API reference and diagrams.** The package ships a `py.typed` marker and every
  public module, class, and function has a Google-style docstring (enforced by
  ruff `D`). `pdoc` generates an HTML API reference into `build/docs`; the
  `docs-build` CI job fails on warnings and publishes to GitHub Pages from `main`
  and tags. The README gains a sequence diagram for update → router → command →
  API call → DB write alongside the architecture flowchart.

- **Docs consistency checks.** `tests/test_docs_consistency.py` runs in the
  normal suite and fails when the prose drifts from the single sources of truth:
  every registered command has a `### /<name>` section in `docs/commands.md`
  with Examples and Edge cases; the README command table matches the registry;
  every `Settings` key is documented and `config/config.example.toml` validates
  with placeholder secrets; `nacho-moderator --help` equals the golden
  `docs/cli-help.txt`; `setMyCommands` obeys Telegram's limits and the group
  scope is a subset of the admin scope; every TOML and `title="ci"` snippet is
  valid; no bare TODO/FIXME; markdown tables are consistent; relative links
  resolve; and (in CI) the CHANGELOG changed when `src/` changed. Each check has
  a fixture test for its failure mode. `docs/commands.md` is restructured into
  one section per command.

- **`/promote`, `/demote` and `/start`.** The three commands present in the
  original command table but never ported. `/promote` grants only a limited,
  safe set of rights (delete messages, restrict members, pin messages) and never
  `can_promote_members`/`can_change_info`/`can_manage_chat`, with an optional
  custom title (16 characters, HTML-escaped when echoed). `/demote` clears every
  right and refuses the owner, the bot itself, and (via Telegram's own rejection)
  an administrator the bot did not promote. `/start` replies with the same text
  as `/help` and is the only command besides `/help` that works in private chats.
  The registry now holds 31 commands.

- **Soak test with real health gates.** The bot now records per-update handler
  latency and event-loop lag in bounded histograms, dumped as one `soak-metrics`
  line on `SIGUSR1` and at shutdown. `scripts/run_soak.sh` has a 30-second smoke
  mode (every PR, with `PYTHONASYNCIODEBUG=1 -X dev -W error::ResourceWarning`)
  and a 600-second nightly mode; it samples RSS and FD count every 5 seconds and
  fails on a p99 latency above 50 ms, loop lag above 100 ms
  ([ADR 0002](docs/adr/0002-python-loop-lag-budget.md)), RSS growth above 10 %
  after a 60 s warm-up, a rising FD count, any traceback or un-awaited-task
  warning, or a non-zero exit. The pass/fail logic is `scripts/soak_check.py`,
  unit-tested against fixture logs.

- Feature-parity port of the C bot:
  - 28 commands across moderation, warnings, per-chat settings, welcome/rules,
    and automod, driven by a single command registry that produces both `/help`
    and the two `setMyCommands` scopes.
  - Command parsing and target resolution (reply, numeric id, `@username`) with
    duration parsing (`30s`/`10m`/`2h`/`7d`/`1w`).
  - Administrator cache (10-minute TTL) and the caller/bot permission matrix.
  - Automod: message locks, word/regex blacklist (with a match-time guard), and
    antiflood, run before command dispatch.
  - Join captcha with a timed kick, and a 30-second sweeper that lifts expired
    temporary bans and mutes (survives restart).
  - Outbound rate limiting (global 25/s, per-chat 15/min, bulk 1/s) with 429
    pause handling and exponential-backoff retries for idempotent methods.
  - Polling and webhook modes; the webhook listener rejects a bad secret with 403.
  - Graceful shutdown with a bounded drain on `SIGINT`/`SIGTERM`.
  - SQLite persistence (WAL) via `aiosqlite` with schema migrations.
- A `pytest` suite (unit tests plus a mock Bot API server) with an 85% coverage
  floor, `ruff` and `mypy --strict` configured in `pyproject.toml`.
- Rewritten documentation (`docs/`), README, and systemd/Docker packaging.

### Removed

- The C11 implementation, `liburing` event loop, OpenSSL memory-BIO TLS, the
  CMake toolchain, Doxygen docs, the `.def` X-macro tables, and the C1–C18
  consistency-check tooling. The C code remains reachable at the
  `c-implementation-final` tag.

## Historical (C11 implementation, superseded)

The entries below describe the previous C11/`liburing` implementation.

### Added

- README cover image (`docs/assets/cover.png`), checked by consistency check C17.

### Security

- Webhook mode now refuses to start unless `webhook.secret` is set to at least 16
  characters, and an empty expected secret rejects every request instead of
  accepting them. Previously `webhook_verify_secret()` accepted any request when
  no secret was configured, so anyone able to reach the listen port could forge
  updates and impersonate administrators. The requirement is enforced in
  `config_validate()` (shared by `--check-config` and startup) via a new
  `CFG_REQUIRED_IF(...)` mechanism in `src/config.def` (`src/config.c`,
  `src/webhook.c`, `src/main.c`). Unit tests cover empty, short, wrong, missing
  and correct secrets; an integration test proves webhook mode with no secret
  fails to start and a header-less request gets 403.
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
