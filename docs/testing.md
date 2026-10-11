# Testing

The test strategy follows SPEC-ADDENDUM-TOOLCHAIN A5 and SPEC-ADDENDUM-SSOT S7.

## Unit tests (cmocka)

Fast (< 5 s), no network, no real clock, no threads where possible. One file per
module under `tests/unit/`. Injectable interfaces (`clock_ops`, `net_ops`,
`db_ops`, `tg_api_ops`) and a `fake_ring` test double keep logic tests hermetic.

```sh
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure
```

## Integration tests

Real event loop and io_uring against the Python mock Telegram server
(`tests/mock/mock_tg_server.py`) over plain HTTP (`api_base = http://127.0.0.1:<port>`)
and a real SQLite file. Added with M2+.

M2 adds `tests/integration/test_integration_polling.py`: the real `tgbot` is run
against a scripted mock that injects a 429 on the first `getUpdates`; the test
asserts the poll is retried at the same offset and then advances past the received
update id, and that `allowed_updates` is sent explicitly. The mock supports
scenarios (`tests/mock/scenarios/*.json`) and fault injection (429, 5xx, malformed
JSON, connection reset), and records every call as JSONL for assertions.

M3 adds `tests/integration/test_integration_router.py`: the mock injects a scripted
`/help` message; the test asserts the router resolves the caller via
`getChatAdministrators` and replies with `sendMessage`, that `setMyCommands` is
registered at startup, and that a non-command message produces no reply. It uses a
real temporary SQLite file (`DB_PATH` in the test env).

M4 adds `tests/integration/test_integration_moderation.py`: scripted `/ban`,
`/kick`, `/purge` and `@username` scenarios assert the exact outbound calls.

M5 adds `tests/integration/test_integration_m5.py`: `/warn` reaching the default
limit triggers the configured action; a persisted welcome is delivered on join
(`getChatMemberCount` + rendered `sendMessage`); and a `/settings` panel press is
answered and re-rendered with `answerCallbackQuery` + `editMessageText`. Scenarios
live in `tests/mock/scenarios/{warn_limit_action,welcome_join,welcome_join_part2,settings_panel}.json`.

M6 adds `tests/integration/test_integration_m6.py`: a locked media type is deleted
while plain text is not; an antiflood burst mutes the sender and deletes the burst;
enabling the captcha then a join restricts the member and sends an inline-keyboard
prompt; and a captcha callback pressed by the wrong user is rejected. Scenarios
live in `tests/mock/scenarios/{automod_locks,automod_antiflood,captcha_join,captcha_join_part2,captcha_wrong_user}.json`.

## Fuzz tests

libFuzzer harnesses under `tests/fuzz/`. CI runs each target for 60 s per PR and
30 min nightly; crashers are committed under `tests/fuzz/regressions/`.

```sh
cmake --preset fuzz && cmake --build --preset fuzz
./build/fuzz/tests/fuzz/fuzz_smoke -max_total_time=60
```

## Soak / load tests

`scripts/run_soak.sh` replays 1,000 updates/s for 10 minutes under the `asan`
build and checks latency, loop stalls, RSS/FD flatness, and LSan. Added with M2+.

## Coverage

```sh
cmake --preset coverage && cmake --build --preset coverage && ctest --preset coverage
cmake --build --preset coverage --target coverage
```

Thresholds: ≥ 80 % line coverage overall, ≥ 90 % for `http`, `tg_api`
(limiter/retry), `perms`, `automod`, and `util`.

## Consistency checks (SSOT)

`scripts/check_consistency.py` verifies that documentation and generated artifacts
match the tables exported by the binary. All checks C1–C18 are implemented (SPEC
ADDENDUM-SSOT S7).

| ID | Check | Status |
|---|---|---|
| C1 | `docgen.py --check`: generated blocks up to date | implemented |
| C2 | `docs/commands.md` headings match the command table | implemented |
| C3 | each non-hidden command has a `docs/commands/<name>.md` snippet | implemented |
| C4 | `docs/configuration.md` documents every config key | implemented |
| C5 | `config/config.example.toml` matches `--dump-config-example` | implemented |
| C6 | every TOML fenced block in docs passes `--check-config` | implemented |
| C7 | README `title="ci"` shell snippets parse and use known presets | implemented |
| C8 | `--help` matches `tests/golden/help.txt` and `docs/building.md` | implemented |
| C9 | every `api_methods.def` method has a mock handler and a docs row | implemented |
| C10 | commands appear in `tgbot.1.scd`; config keys in `tgbot.toml.5.scd` | implemented |
| C11 | `setMyCommands` payload constraints (names, briefs, scope subset) | implemented |
| C12 | every `@ingroup` is defined in `groups.dox`; every src file has `@file` | implemented |
| C13 | no `TODO`/`FIXME` without an issue reference | implemented |
| C14 | `CHANGELOG.md` `Unreleased` changed when `src/` changed (PR context) | implemented |
| C15 | `commands.def`/`config.def` changes come with tests and docs | implemented |
| C16 | metrics in `metrics.def` and errors in `errors.def` are documented | implemented |
| C17 | relative links and anchors inside `docs/` and README resolve | implemented |
| C18 | forbidden terminology from `docs/terminology.txt` is absent | implemented |

```sh
scripts/check_consistency.py --bin build/debug/tgbot
scripts/check_consistency.py --list
scripts/check_consistency.py --only C3,C4
scripts/check_consistency.py --format github   # CI annotations
scripts/check_consistency.py --bin build/debug/tgbot --base origin/main  # C14/C15
```

The script exits 0 when all selected checks pass, 1 when a check fails, and 2 on a
tool error (missing binary, invalid JSON), so CI can distinguish "inconsistent"
from "broken". C14 and C15 are PR-context checks: without `--base <ref>` they pass
vacuously (the local/CTest run), and CI passes `--base origin/main`. It is
registered as the CTest test `consistency` and has its own tests in
`tests/scripts/test_check_consistency.py` (a passing and a failing fixture per
check).

## No blocking syscalls on the main thread

```sh
scripts/check_blocking.sh build/asan/tgbot --self-test
```

Uses `strace` to fail if the main thread calls blocking `read`, `connect`, `poll`,
`futex` wait, or touches SQLite/file I/O after the event loop reaches steady
state. `--self-test` runs the loop with a timer and no network or token, and
prints a `loop: ready` sentinel on stderr that marks the start of the checked
region (startup library/config I/O happens before it and is expected).

## Integration (mock Telegram server)

`tests/mock/mock_tg_server.py` answers `getMe`/`getUpdates` over plain HTTP.
`tests/integration/test_integration_getme.py` points the real `tgbot` at it via
`BOT_TOKEN`/`API_BASE` and asserts a successful `getMe` (registered as the CTest
test `integration-getme`).
