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
match the tables exported by the binary. Checks C1–C5 and C9 are implemented now;
the rest are added as their features land (SPEC-ADDENDUM-SSOT S10).

| ID | Check | Status |
|---|---|---|
| C1 | `docgen.py --check`: generated blocks up to date | implemented |
| C2 | `docs/commands.md` headings match the command table | implemented |
| C3 | each non-hidden command has a `docs/commands/<name>.md` snippet | implemented |
| C4 | `docs/configuration.md` documents every config key | implemented |
| C5 | `config/config.example.toml` matches `--dump-config-example` | implemented |
| C9 | every `api_methods.def` method has a mock handler and a docs row | implemented |
| C6–C18 | see SPEC-ADDENDUM-SSOT S7 | pending their features |

```sh
scripts/check_consistency.py --bin build/debug/tgbot
scripts/check_consistency.py --list
scripts/check_consistency.py --only C3,C4
scripts/check_consistency.py --format github   # CI annotations
```

The script exits 0 when all selected checks pass, 1 when a check fails, and 2 on a
tool error (missing binary, invalid JSON), so CI can distinguish "inconsistent"
from "broken". It is registered as the CTest test `consistency` and has its own
tests in `tests/scripts/test_check_consistency.py`.

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
