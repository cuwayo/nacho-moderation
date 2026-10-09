# SPEC ADDENDUM: Toolchain, Build, Testing, CI

> This addendum supersedes: the "Build" row in section 3, the `/tests` entry in section 8,
> and section 11 acceptance item 2 (Valgrind is NOT usable with io_uring; use sanitizers).

## A1. Toolchain

| Purpose | Tool | Version / Notes |
|---|---|---|
| Compilers | GCC >= 12 AND Clang >= 16 | Both must build cleanly. Clang is required for libFuzzer. |
| Build system | CMake >= 3.25 + Ninja | Out-of-tree builds only. `CMAKE_EXPORT_COMPILE_COMMANDS=ON`. |
| Dependencies | System packages (`liburing-dev`, `libssl-dev`, `libsqlite3-dev`, `libpcre2-dev`) found via `pkg-config`. `yyjson` and `cmocka` vendored under `/third_party` (git submodule or `FetchContent` with pinned commit hash). |
| Formatter | `clang-format` | `.clang-format` committed (LLVM base, 4-space indent, 100 cols, Linux-style braces). CI fails on diff. |
| Static analysis | `clang-tidy`, `cppcheck`, GCC `-fanalyzer` | `.clang-tidy` committed. Enable `bugprone-*`, `cert-*`, `clang-analyzer-*`, `misc-*`, `performance-*`. Warnings are errors in CI. |
| Test framework | **cmocka** | Supports mocks (`will_return`, `expect_*`, `__wrap_` linker wrapping). |
| Fuzzing | libFuzzer (Clang) + optional AFL++ | See A5. |
| Coverage | `gcov` + `lcov` / `gcovr` | HTML + Cobertura XML output. |
| Dynamic analysis | ASan, UBSan, LSan, TSan | See A4. Valgrind: NOT supported (no io_uring). |
| Tracing / perf | `strace`, `perf`, `bpftrace`, `ltrace` | Used for the "no blocking syscalls" check. |
| Git hooks | `pre-commit` | clang-format, clang-tidy (changed files), codespell, trailing whitespace. |
| Container | Dockerfile for reproducible CI build | Note: Docker's default seccomp profile may block io_uring; run tests with a custom seccomp profile or `--security-opt seccomp=unconfined`, or natively on the runner. |

Minimum runtime/CI kernel: **Linux >= 5.15** (target 6.1+). The build must detect missing io_uring features at runtime (`io_uring_get_probe`) and fall back per section 4.3.

## A2. Repository Layout (additions)

```
/.clang-format
/.clang-tidy
/.pre-commit-config.yaml
/.github/workflows/ci.yml
/CMakeLists.txt
/CMakePresets.json
/cmake/                  # Sanitizers.cmake, Warnings.cmake, FindLiburing.cmake
/third_party/            # yyjson, cmocka (pinned)
/src/
/include/                # public headers if any
/tests/
  CMakeLists.txt
  unit/                  # test_*.c, one file per module
  integration/           # tests driving the real loop against the mock server
  fuzz/                  # fuzz_*.c harnesses
  mock/
    mock_tg_server.py    # scripted fake Telegram Bot API (plain HTTP)
    scenarios/*.json     # scripted update sequences + expected outbound calls
  fixtures/              # recorded real Telegram JSON payloads (tokens/IDs redacted)
/scripts/
  run_soak.sh
  check_blocking.sh
  coverage.sh
/packaging/
  tgadminbot.service     # systemd unit (hardened)
  Dockerfile
```

## A3. Build Configuration

### CMake presets (`CMakePresets.json`)
| Preset | Flags |
|---|---|
| `debug` | `-O0 -g3 -fno-omit-frame-pointer` + warnings |
| `asan` | `debug` + `-fsanitize=address,undefined -fno-sanitize-recover=all` |
| `tsan` | `debug` + `-fsanitize=thread` |
| `coverage` | `debug` + `--coverage` |
| `fuzz` | Clang, `-fsanitize=fuzzer,address,undefined` for fuzz targets only |
| `release` | `-O2 -g -DNDEBUG -flto` + hardening flags |

### Warning flags (all presets)
```
-std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
-Wformat=2 -Wformat-security -Wnull-dereference -Wstrict-prototypes
-Wmissing-prototypes -Wcast-align -Wundef -Wvla -Werror
```

### Hardening flags (release)
```
-D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection
-fcf-protection=full -fPIE -pie -Wl,-z,relro,-z,now,-z,noexecstack
```

### Targets
- `tgbot` (executable), `libtgbot_core` (static lib containing everything except `main.c`, so tests can link it)
- `unit_tests`, `integration_tests`, `fuzz_*`
- Custom targets: `format`, `format-check`, `tidy`, `cppcheck`, `coverage`, `docs`
- `ctest` must run all unit + integration tests: `ctest --preset asan --output-on-failure`

### Build rules
- No source file may include a header it does not use (include-what-you-use style).
- All I/O-touching functions go through thin interfaces (function-pointer vtables or link-time `--wrap`) so they can be mocked: `net_ops`, `clock_ops`, `db_ops`, `tg_api_ops`.
- Time source is injectable (`clock_now_ms()`), never call `time()`/`clock_gettime()` directly in logic modules (needed for deterministic flood/expiry/captcha tests).
- `random` source injectable (captcha nonce) for deterministic tests.

## A4. Sanitizers & Dynamic Analysis

| Tool | When | Notes |
|---|---|---|
| ASan + LSan + UBSan | Every unit/integration test run in CI (`asan` preset) | `ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1:detect_stack_use_after_return=1`, `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1` |
| TSan | Integration tests that exercise the DB worker thread + eventfd handoff | Separate build; ASan and TSan cannot be combined. Zero reports allowed. |
| Valgrind | **Do not use** | Does not implement io_uring syscalls. Only usable on pure-logic unit tests that do not touch the ring (optional). |
| `strace -f -c` / `perf trace` | `scripts/check_blocking.sh` | Fails if the main thread calls blocking `read`, `connect`, `poll`, `futex` wait, or any SQLite/file I/O during the soak run. |
| io_uring trace | `bpftrace` / `perf` on `io_uring:*` tracepoints | Optional; used to verify batching (submits per loop iteration) |

## A5. Test Strategy

### Test pyramid
1. **Unit tests (cmocka)**, fast (< 5 s total), no network, no real clock, no threads where possible.
2. **Integration tests**, real event loop + real io_uring + real TLS-less HTTP against the Python mock server (`api_base=http://127.0.0.1:<port>`), real SQLite on a temp file.
3. **Fuzz tests**, parsers and anything handling untrusted input.
4. **Soak/load tests**, scripted, nightly in CI.

### A5.1 Unit test coverage matrix (each row = at least one test file)
| Module | Must cover |
|---|---|
| `util` (duration parser) | `30s 10m 2h 7d 1w`, no suffix, zero, overflow, uppercase, garbage, empty |
| `util` (arena) | alloc, alignment, reset, exhaustion behavior |
| `util` (html escape) | `& < > "`, UTF-8 passthrough, truncation at 4096, buffer-boundary cases |
| `http` | status line parse, headers case-insensitivity, Content-Length body, **chunked** (split across arbitrary byte boundaries), keep-alive vs `Connection: close`, oversized header/body rejection, partial feeds (1 byte at a time) |
| `tg_types` | JSON -> structs for message, edited_message, callback_query, chat_member, my_chat_member using fixtures; missing/null/extra fields; huge ids (int64 negative chat ids like `-100...`) |
| `tg_api` rate limiter | token bucket refill with fake clock, global + per-chat limits, queue ordering, 429 `retry_after` pause, backoff schedule 1/2/4/8/16/30 |
| `router` | command parsing (`/ban@MyBot`, case, args split, reply-target, `@username`, numeric id), ignoring commands addressed to other bots |
| `perms` | permission matrix: {owner, admin w/ and w/o rights, member, sudo} x {each command}; bot lacking rights; target is owner/admin/self |
| `automod` flood | sliding window boundaries, burst, multiple users isolated, window expiry (fake clock) |
| `automod` blacklist | plain + regex, case-insensitivity, ReDoS pattern rejected/limited |
| `automod` locks | bitmask set/unset per type, message-type detection |
| `captcha` | nonce generation, callback_data format <= 64 bytes, wrong-user rejection, timeout path, double-click |
| `db` | migrations from v0 -> latest, upsert users, warn add/remove/count, expiring-actions query, concurrent job ordering |
| `cache` | TTL expiry, eviction, collision handling |
| `tls` adapter | memory-BIO pump with a loopback `SSL` peer: handshake, partial records, close_notify |

### A5.2 Mocking approach
- Use cmocka + `-Wl,--wrap=<symbol>` for libc/OS-level seams where needed.
- Prefer injectable interfaces (A3) over linker wraps for project code.
- Provide a `fake_ring` test double implementing the same interface as `loop.c` for logic tests that only need to capture "submitted operations" and feed back synthetic CQE results (including `-EINTR`, `-ECANCELED`, short reads, `-ECONNRESET`).

### A5.3 Integration tests (mock Telegram server)
`mock_tg_server.py` (Python 3, stdlib `http.server`/`asyncio`, test-only dependency) must:
- Serve `getMe`, `getUpdates`, `sendMessage`, `banChatMember`, `unbanChatMember`, `restrictChatMember`, `promoteChatMember`, `deleteMessage`, `deleteMessages`, `pinChatMessage`, `getChatAdministrators`, `answerCallbackQuery`, `setMyCommands`.
- Load a scenario JSON: ordered list of `{inject_update, expect_calls[], respond_with}`.
- Support fault injection: HTTP 429 + `retry_after`, 500, connection reset mid-body, slow response, chunked responses, malformed JSON.
- Record all received calls to a JSONL file for assertions.

Required scenarios:
1. Admin `/ban @user 10m reason` -> `banChatMember` with correct `until_date`, log message sent to log channel.
2. Non-admin `/ban` -> no API call, error reply.
3. Bot lacks `can_restrict_members` -> clear error reply.
4. `/kick` continuation: ban then unban, even when the first call returns 500 once (retry).
5. 429 on `sendMessage` -> no request to that bucket before `retry_after`, other chats unaffected.
6. Warn limit reached -> configured action executed, warns cleared.
7. Captcha: join -> restrict -> button callback correct user (pass) / wrong user (rejected) / timeout (kick + cleanup).
8. Antiflood: burst triggers mute and deletes burst messages.
9. Temp ban expiry survives process restart (kill bot, restart with same DB, clock advanced via `--fake-time-offset` test flag).
10. `/purge` over 250 messages -> 3 `deleteMessages` batches.
11. Polling offset advances correctly; duplicate update ids ignored; connection dropped mid-long-poll -> reconnect without losing updates.
12. Graceful shutdown on SIGTERM drains in-flight requests and exits 0 within 10 s.

### A5.4 Fuzz targets (`tests/fuzz`)
| Target | Input |
|---|---|
| `fuzz_http_response` | raw bytes into HTTP response parser (chunked, headers, split feeds) |
| `fuzz_update_json` | arbitrary bytes into the update parser |
| `fuzz_command_parser` | arbitrary message text into command/target/duration parser |
| `fuzz_html_escape` | arbitrary text into escape + truncation |
| `fuzz_callback_data` | arbitrary callback_data strings |
- Seed corpus from `tests/fixtures`. CI runs each target for 60 s per PR; nightly runs 30 min each.
- Crashers are committed to `tests/fuzz/regressions/` and replayed as unit tests.

### A5.5 Soak / load tests
- `scripts/run_soak.sh`: replay 1,000 updates/s for 10 min against the mock server under the `asan` build.
- Collect: p50/p99 handler latency (internal histogram exposed on SIGUSR1 or debug log), max event-loop iteration time, RSS growth (must be flat +/- 5%), open FD count (flat), LSan report at exit (empty).
- Event-loop stall detector (debug builds): log WARN when a loop iteration takes > 5 ms; the soak test fails if any occur.

### A5.6 Coverage targets
- Line coverage >= 80% overall, >= 90% for `http`, `tg_api` (limiter/retry), `perms`, `automod`, `util`.
- Branch coverage reported, not gated.
- Coverage report uploaded as CI artifact.

## A6. CI Pipeline (`.github/workflows/ci.yml`)

Runner: `ubuntu-24.04` (kernel supports io_uring). Jobs:

| Job | Steps |
|---|---|
| `lint` | `format-check`, `clang-tidy`, `cppcheck`, `codespell` |
| `build-matrix` | {gcc, clang} x {debug, release}: build with `-Werror` |
| `test-asan` | `asan` preset: unit + integration via `ctest` |
| `test-tsan` | `tsan` preset: integration tests touching DB worker |
| `coverage` | `coverage` preset, `gcovr`, enforce thresholds, upload artifact |
| `fuzz-smoke` | Clang `fuzz` preset, 60 s per target, upload any crash |
| `nightly-soak` | scheduled: soak test (A5.5) + 30 min fuzzing + `check_blocking.sh` |
| `release` | on tag: build release binary, checksums, Docker image |

All jobs cache dependencies (`ccache`). PRs cannot merge with any job red.

## A7. Coding Standards

- C11, no GNU extensions in logic modules (allowed in `loop.c`/`tls.c` if needed, documented).
- Naming: `snake_case` functions/vars, `module_verb_noun()` (e.g. `tg_api_send_message`), types suffixed `_t`, macros/enums `UPPER_SNAKE`.
- Every function that can fail returns `int` (0 = ok, negative errno) or a tagged result; no silent failures.
- Ownership documented in header comments: who allocates, who frees, lifetime vs the per-update arena.
- No global mutable state except the loop singleton and logger; pass context pointers.
- No `strcpy/strcat/sprintf/gets/atoi`; use `snprintf`, length-aware helpers, `strtol` with checks.
- Header guards or `#pragma once` (pick one, be consistent); each header self-contained.
- Every io_uring state transition has a comment `/* STATE: X -> Y on event Z */`.
- Max function length ~80 lines; max cyclomatic complexity 15 (enforced via clang-tidy `readability-function-cognitive-complexity`).

## A8. Observability & Ops

- Log format: `ts level module chat=<id> user=<id> msg` (no tokens; redact `bot<token>` in URLs).
- Counters (dumped on `SIGUSR1` and optionally exposed at `127.0.0.1:9100/metrics` in Prometheus text format): updates received, API calls by method/status, 429 count, retries, queue depth, DB job latency, loop max-iteration time, active connections.
- systemd unit hardening: `NoNewPrivileges=yes ProtectSystem=strict ProtectHome=yes PrivateTmp=yes ReadWritePaths=/var/lib/tgadminbot RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX MemoryDenyWriteExecute=yes SystemCallFilter=@system-service io_uring_setup io_uring_enter io_uring_register` (explicitly allow io_uring syscalls), `Restart=on-failure`.

## A9. Definition of Done (per milestone / PR)

- [ ] Builds warning-free with GCC and Clang (`-Werror`)
- [ ] `clang-format`, `clang-tidy`, `cppcheck` clean
- [ ] New code has unit tests; parsers/untrusted-input code has a fuzz target
- [ ] `ctest` green under `asan`; TSan green if threads touched
- [ ] Coverage thresholds met
- [ ] No new blocking syscall on main thread (`check_blocking.sh` passes)
- [ ] README/docs updated (build steps, config keys, new commands)
- [ ] Commit messages follow Conventional Commits (`feat:`, `fix:`, `test:`, `chore:`)

## A10. Instructions to the Implementing Agent (additions)

- Set up the toolchain skeleton FIRST (CMake presets, sanitizers, clang-format/tidy, cmocka, one trivial passing test, CI workflow) before writing feature code (this is "milestone M0").
- Write the unit tests for each module in the same commit as the module (test-first for parsers and limiter).
- Never disable a sanitizer or warning to make something pass; fix the root cause or ask.
- When a test needs time, randomness, network, or DB, use the injectable interfaces from A3, never real ones.
- After each milestone, print: exact commands to build, run all tests, run the fuzzers, and produce the coverage report.
