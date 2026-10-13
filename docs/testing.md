# Testing

The test suite uses `pytest`. Tests exercise *this project's* logic — parsers,
services, handlers, the permission matrix, the rate limiter, and the automod
pipeline — against in-memory doubles and a fake bot that records the calls the
code makes. The suite never drives the aiogram library or the network.

```sh
uv sync
uv run pytest                 # all tests
uv run pytest -q              # quiet
uv run pytest --cov           # with a coverage report (global floor 90%)
uv run pytest tests/unit/test_cmd_moderation.py -v
```

The suite runs under **`pytest-asyncio`** (auto mode, one session-scoped event
loop — matching production's single long-lived loop and avoiding an aiosqlite
worker-thread teardown race), **`pytest-randomly`** (each run shuffles test order
and seeds `hypothesis`; pass `-p no:randomly` for a deterministic order),
**`pytest-timeout`** (a hung test fails after 30 s), and optionally
**`pytest-xdist`** (`-n auto`). Property tests use **`hypothesis`**; the
injectable clock is used instead of `freezegun`.

The whole suite also passes with asyncio's debug mode on, which is what CI runs:

```sh
PYTHONASYNCIODEBUG=1 uv run python -X dev -W error -m pytest -q
```

## Layout

```text
tests/
  conftest.py        # fixtures: in-memory db, fake clock, FakeBot, fake admins
  helpers.py         # builders for messages and command contexts
  integration/       # the real bot process driven against the mock server
  mock/              # a standalone mock Telegram Bot API server
  unit/              # one file per module / behaviour area
  test_docs_consistency.py   # docs-vs-registry/config/link checks
```

## Test doubles

- **`FakeBot`** records every call the code makes (`send_message`,
  `ban_chat_member`, ...) and returns small canned values, so assertions are
  about our decisions, not about HTTP.
- **`FakeClock`** makes TTLs, flood windows, and captcha deadlines deterministic.
- **In-memory SQLite** (`Database.connect(":memory:")`) keeps DB tests hermetic.

## What is covered

| Area | File |
| --- | --- |
| Duration parsing, HTML escaping | `test_models.py` |
| Token buckets, retry policy, backoff | `test_transport.py` |
| Rate-limit/retry session middleware | `test_ratelimit_middleware.py` |
| Command parsing and target resolution | `test_router.py` |
| Permission matrix | `test_perms.py` |
| Administrator cache TTL/invalidation | `test_admin_cache.py` |
| Database schema, warns, actions, captcha | `test_db.py` |
| Automod (locks/blacklist/antiflood) | `test_automod.py` |
| Captcha service | `test_captcha_service.py` |
| Expiry sweeper | `test_expiry.py` |
| Command handlers | `test_cmd_*.py` |
| Dispatch, guards, automod middleware | `test_dispatch.py` |
| Join and callback handlers | `test_join_callback.py` |
| Config loading and precedence | `test_config.py` |
| CLI entry point | `test_cli.py` |
| Log redaction (property test) | `test_logging_redaction.py` |
| Runtime metrics | `test_metrics.py` |
| Soak pass/fail logic | `test_soak_check.py` |
| Per-module coverage floors | `test_check_coverage.py` |
| Docs consistency (P8) | `test_docs_consistency.py` |

## Docs consistency

`tests/test_docs_consistency.py` is part of the normal suite. It treats the
command registry and the `Settings` model as the single sources of truth and
fails when the prose docs drift from them:

- every registered command has a `### /<name>` section in `docs/commands.md`
  with **Examples** and **Edge cases**, and no section names an unregistered
  command;
- the README command table lists exactly the registry's commands;
- every `Settings` key appears in `docs/configuration.md`, and
  `config/config.example.toml` validates under `--check-config` with placeholder
  secrets. The example is validated from a **`chmod 600` copy** in `tmp_path`, not
  in place: git checks the tracked file out 0644, and the P2 permission check
  (correctly) refuses a group/other-readable secret. A regression test materialises
  a fresh checkout under `umask 022` and asserts the copy validates;
- `nacho-moderator --help` equals the golden `docs/cli-help.txt` and the flags
  are documented in the README;
- `setMyCommands` payloads obey Telegram's limits and the group scope is a subset
  of the admin scope;
- every TOML snippet validates and every `title="ci"` bash snippet parses and
  references only existing scripts;
- no bare `TODO`/`FIXME`, markdown tables have consistent cell counts, and
  relative links resolve;
- (CI only) the CHANGELOG `Unreleased` section changed when `src/` changed
  (`skip-changelog` label bypasses it).

Each check is a small pure function, and `TestFixtureChecks` exercises every
failure mode against deliberately broken fixture text. Regenerate the golden
`--help` with `uv run nacho-moderator --help > docs/cli-help.txt`.

## Mock Bot API server (manual / soak)

`tests/mock/mock_tg_server.py` is a standalone plain-HTTP server that speaks
enough of the Bot API to run the real bot without a token or TLS. Point the bot
at it with `API_BASE=http://127.0.0.1:<port>`.

```sh
python3 tests/mock/mock_tg_server.py --port 8081 --scenario tests/mock/scenarios/ban_reply.json --log calls.jsonl
MODE=polling API_BASE=http://127.0.0.1:8081 BOT_TOKEN=123456:TEST uv run nacho-moderator
```

It supports scripted scenarios (`tests/mock/scenarios/*.json`) and fault
injection (429 with `retry_after`, 5xx, malformed JSON, connection reset), and
records every received call as JSONL.

With `--soak SECONDS` a background feeder queues synthetic updates at `--rate`/s
for that long; each `getUpdates` serves up to `--soak-batch` of them (default
100). The pending queue is capped at `SOAK_MAX_BACKLOG_SECONDS` (5 s) of backlog
at the requested rate, so a bot that cannot keep up has its oldest queued updates
dropped (with a warning) rather than growing the queue without limit.

## Coverage

`uv run pytest --cov` reports line and branch coverage and fails below the
**global floor of 90 %** configured in `pyproject.toml`. Per-module floors are
enforced by `scripts/check_coverage.py`, which reads `coverage.json`:

```sh
uv run pytest --cov --cov-report=json
uv run python scripts/check_coverage.py coverage.json
```

| Module group | Floor |
| --- | --- |
| `transport/*`, `services/perms.py`, `services/automod.py`, `commands/args.py`, `config.py`, `logging_setup.py` | 95 % |
| `handlers.py`, `main.py` | 85 % |

## Property tests (hypothesis)

`hypothesis` drives the parsers and the redaction guard: the duration parser, the
command/target/arg parser, the callback-data parser, HTML escaping/truncation,
log redaction, the webhook listen parser, the blacklist regex guard (no
catastrophic backtracking), and the rate-limiter token bucket against the fake
clock. CI randomizes the `hypothesis` seed.

## Security checks

- **`pip-audit`** fails the build on a known vulnerability in a dependency.
- **`ruff`'s `S` rules** (bandit) run as part of lint.
- **`detect-secrets`** scans the tree against `.secrets.baseline` in pre-commit
  and CI.

## Soak test

`scripts/run_soak.sh` runs the real bot against the mock server's synthetic
update feed and checks that it stays healthy. Two modes:

```sh
bash scripts/run_soak.sh smoke      # 30 s @ 1000/s, production flags, full gate (every PR)
bash scripts/run_soak.sh nightly    # 600 s @ 1000/s, production flags, full gate
bash scripts/run_soak.sh hygiene    # 30 s, PYTHONASYNCIODEBUG + -X dev; hygiene only
```

Timing budgets (handler latency, loop lag) are only meaningful with production
flags: `PYTHONASYNCIODEBUG` and `-X dev` inflate them by an order of magnitude.
So `smoke` and `nightly` run the bot with production flags and enforce every
budget, while `hygiene` runs with the debug flags and checks **hygiene only**
(tracebacks, un-awaited coroutines, resource warnings) — it reports the timing
numbers without asserting them. CI runs `smoke` and `hygiene` on every PR and
`nightly` on a schedule.

The bot measures per-update **handler latency** and **event-loop lag** and logs a
`soak-metrics ...` line on shutdown (and on `SIGUSR1`), plus the ten largest
`soak-lag-event` lines (each with its elapsed time). Lag is split into a
**warm-up** window (the first 60 s, matching the RSS warm-up) and **steady state**,
so a startup transient does not fail the steady-state budgets while a genuine
startup hang still does. The line includes `uptime_seconds` — the bot's own
processing window, the denominator for the achieved rate. The mock's soak feed
serves up to `--soak-batch` updates per `getUpdates` (default 100, Telegram's
real limit) so the achieved rate tracks the requested rate; its pending queue is
capped at a few seconds of backlog, and a bot that cannot keep up has the oldest
queued updates dropped (with a warning) instead of growing the queue without
bound. The harness samples RSS and FD count every 5 s. `scripts/soak_check.py` —
which owns the pass/fail thresholds and is unit-tested in
`tests/unit/test_soak_check.py` — fails the run on:

- handler-latency p99 > 50 ms;
- loop lag: warm-up max > 1000 ms (a startup hang); steady-state p99 > 100 ms or
  steady-state max > 250 ms (see
  [`docs/adr/0002-python-loop-lag-budget.md`](adr/0002-python-loop-lag-budget.md));
- achieved update rate < 80 % of the requested rate (the minimum handler-sample
  count is derived from the run: `0.8 * rate * seconds`, never a fixed default);
- RSS growth > 10 % after a 60 s warm-up (linear-regression slope);
- FD count not flat;
- any traceback, `Task exception was never retrieved`, or `was never awaited`;
- a non-zero bot exit.

Environment overrides: `SOAK_SECONDS`, `SOAK_RATE`, `SOAK_BATCH`.
