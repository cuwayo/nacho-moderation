# nacho-moderator

An **io_uring-based Telegram admin/moderator bot** written in **C11**. All network
I/O runs asynchronously through `liburing` in a single-threaded event loop;
blocking work (SQLite, DNS) is isolated in worker threads that signal the loop
through an `eventfd`.

> **Status: M5 — warns, settings, welcome/rules, moderation log.** The build
> system, sanitizer and coverage presets, single-source-of-truth tables,
> introspection CLI, and documentation skeleton (M0) are in place; the
> single-threaded io_uring event loop, TCP connect, OpenSSL memory-BIO TLS,
> incremental HTTP client, worker-thread DNS, and a minimal Bot API client that
> performs `getMe` (M1) are implemented; M2 adds reusable keep-alive connections, a
> connection pool, a token-bucket rate limiter with 429 handling and exponential
> backoff, and a `getUpdates` long-poll loop; M3 adds update parsing, a command
> router, a SQLite worker thread with `users`/`chats` upserts, a per-chat
> administrator cache, and the permission model; M4 adds the moderation commands
> (`/ban`, `/unban`, `/kick`, `/mute`, `/unmute`, `/del`, `/pin`, `/unpin`,
> `/purge`), duration parsing, and the expiring-actions sweeper; and M5 adds
> warnings (`/warn`, `/unwarn`, `/warns`, `/resetwarns`), per-chat settings and the
> `/settings` panel, welcome/rules (`/setwelcome`, `/welcome`, `/setrules`,
> `/rules`) with join-time delivery, and the moderation log. Later milestones M6–M7
> (automod/captcha, webhook) follow in order per `docs/SPEC.md` §12.

## Features

- Moderation commands: `/ban`, `/unban`, `/kick`, `/mute`, `/unmute`, `/del`,
  `/pin`, `/unpin`, `/purge`, with `30s`/`10m`/`2h`/`7d`/`1w` durations.
- Warnings: `/warn`, `/unwarn`, `/warns`, `/resetwarns`, with a per-chat limit and
  auto-action (`ban`/`mute`/`kick`).
- Per-chat settings: `/setwarnlimit`, `/setwarnaction`, `/setlog`, `/reload`, and
  an inline-keyboard `/settings` panel.
- Welcome and rules: `/setwelcome`, `/welcome on|off`, `/setrules`, `/rules`, with
  a join welcome rendered from `{first}`/`{mention}`/`{chat}`/`{count}`.
- A moderation log that mirrors every action to a configured channel.
- Temporary bans and mutes that survive a restart and lift automatically.
- Join captcha, antiflood, and blacklist automod (M6).
- Fully asynchronous network I/O through `io_uring`.
- Pooled keep-alive Bot API client with outbound rate limiting and retries.
- `getUpdates` long-polling (webhook mode in M7).
- SQLite persistence on a dedicated worker thread, with an eventfd handoff.
- Single source of truth for commands and config keys, checked in CI.

## Binding specifications

- [`docs/SPEC.md`](docs/SPEC.md) — base spec (architecture, io_uring rules,
  features, data model, security, milestones M1–M7).
- [`docs/SPEC-ADDENDUM-TOOLCHAIN.md`](docs/SPEC-ADDENDUM-TOOLCHAIN.md) — toolchain,
  build, testing, and CI. **Wins on any conflict** and adds milestone **M0**.
- [`docs/SPEC-ADDENDUM-DOCS.md`](docs/SPEC-ADDENDUM-DOCS.md) — documentation
  (Doxygen, README, prose docs, comment conventions, docs CI).
- [`docs/SPEC-ADDENDUM-SSOT.md`](docs/SPEC-ADDENDUM-SSOT.md) — single source of
  truth (X-macro `.def` tables, introspection CLI, `check_consistency.py` C1–C18).

## Requirements

- Linux ≥ 5.15 (target 6.1+), GCC ≥ 12 **and** Clang ≥ 16.
- CMake ≥ 3.25 + Ninja.
- System libraries: `liburing-dev`, `libssl-dev`, `libsqlite3-dev`, `libpcre2-dev`.
- Tools: `clang-format`, `clang-tidy`, `cppcheck`, and (for coverage) `gcovr` or `lcov`.
- Vendored under `third_party/` as pinned git submodules: `cmocka` 1.1.8, `yyjson` 0.13.0.

```sh
# Debian/Ubuntu
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
    liburing-dev libssl-dev libsqlite3-dev libpcre2-dev \
    clang clang-format clang-tidy cppcheck gcovr
```

Clone with submodules:

```sh
git clone --recursive <repo-url>
# or, in an existing checkout:
git submodule update --init --recursive
```

## Quick start

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

The binary exposes read-only introspection dumps that need no token or network:

```sh
build/debug/tgbot --help
build/debug/tgbot --dump-commands-json
build/debug/tgbot --dump-config-json
build/debug/tgbot --dump-tables-json
build/debug/tgbot --dump-config-example
build/debug/tgbot --check-config config/config.example.toml
```

## Configuration

Copy [`config/config.example.toml`](config/config.example.toml) to `config.toml`
and fill in `bot_token`. The sample is **generated** from `src/config.def`; the
full key reference is in [`docs/configuration.md`](docs/configuration.md).

## Commands

The command set lives in `src/commands.def`; the table below is generated from it.

<!-- BEGIN GENERATED: commands-table -->
| Command | Role | Usage | Description |
|---|---|---|---|
| `/help` | ROLE_ANYONE |  | Show available commands |
| `/ban` | ROLE_ADMIN | `[target] [duration] [reason]` | Ban a user from the chat |
| `/unban` | ROLE_ADMIN | `[target]` | Lift a ban |
| `/kick` | ROLE_ADMIN | `[target] [reason]` | Remove a user so they can rejoin |
| `/mute` | ROLE_ADMIN | `[target] [duration] [reason]` | Restrict a user from sending messages |
| `/unmute` | ROLE_ADMIN | `[target]` | Restore a user's permissions |
| `/del` | ROLE_ADMIN |  | Delete the replied-to message and the command |
| `/pin` | ROLE_ADMIN |  | Pin the replied-to message |
| `/unpin` | ROLE_ADMIN |  | Unpin the last pinned message |
| `/purge` | ROLE_ADMIN |  | Delete messages from the replied message to now |
| `/warn` | ROLE_ADMIN | `[target] [reason]` | Warn a user; auto-act at the limit |
| `/unwarn` | ROLE_ADMIN | `[target]` | Remove a user's newest warning |
| `/warns` | ROLE_ANYONE | `[target]` | List your warnings, or a user's (admins) |
| `/resetwarns` | ROLE_ADMIN | `[target]` | Clear all of a user's warnings |
| `/setwarnlimit` | ROLE_ADMIN | `<n>` | Set how many warnings trigger the action |
| `/setwarnaction` | ROLE_ADMIN | `ban|mute|kick` | Set the action taken at the warning limit |
| `/setlog` | ROLE_OWNER | `<chat_id|off>` | Set or clear the moderation log channel |
| `/reload` | ROLE_ADMIN |  | Refresh the cached administrator list |
| `/settings` | ROLE_ADMIN |  | Open the inline settings panel |
| `/setwelcome` | ROLE_ADMIN | `<text>` | Set the join welcome message |
| `/welcome` | ROLE_ADMIN | `on|off` | Enable or disable the join welcome |
| `/setrules` | ROLE_ADMIN | `<text>` | Set the chat rules |
| `/rules` | ROLE_ANYONE |  | Show the chat rules |
| `/lock` | ROLE_ADMIN | `<type> [type...]` | Lock a message type (delete + log) |
| `/unlock` | ROLE_ADMIN | `<type> [type...]` | Unlock a message type |
| `/blacklist` | ROLE_ADMIN | `add|del|list <word|/regex/>` | Manage the chat word/regex blacklist |
| `/antiflood` | ROLE_ADMIN | `<messages> <seconds>|off` | Set the flood threshold, or turn it off |
| `/captcha` | ROLE_ADMIN | `on|off` | Enable or disable join verification |
<!-- END GENERATED: commands-table -->

See [`docs/commands.md`](docs/commands.md) for the full reference.

## Build and test

```sh
# ASan + LSan + UBSan (the gating run in CI)
cmake --preset asan && cmake --build --preset asan
ctest --preset asan --output-on-failure

# TSan (integration tests touching the DB worker; ASan and TSan are separate builds)
cmake --preset tsan && cmake --build --preset tsan
ctest --preset tsan --output-on-failure

# Release (hardened)
cmake --preset release && cmake --build --preset release
```

Build directories are `build/<preset>/` (out-of-tree only).

## Fuzzers

```sh
cmake --preset fuzz && cmake --build --preset fuzz
./build/fuzz/tests/fuzz/fuzz_smoke -runs=1000
```

## Coverage

```sh
cmake --preset coverage && cmake --build --preset coverage
ctest --preset coverage
cmake --build --preset coverage --target coverage   # -> build/coverage/coverage/index.html
```

Targets: ≥ 80 % line coverage overall, ≥ 90 % for `http`, `tg_api` (limiter/retry),
`perms`, `automod`, `util`.

## Lint, formatting, docs, and consistency

```sh
cmake --build --preset debug --target format-check   # clang-format (CI fails on diff)
cmake --build --preset debug --target tidy           # clang-tidy
cmake --build --preset debug --target cppcheck       # cppcheck
cmake --build --preset debug --target docs           # Doxygen HTML (needs doxygen)
cmake --build --preset debug --target docs-check     # Doxygen, warnings are errors
cmake --build --preset debug --target docs-update    # rewrite generated doc blocks
cmake --build --preset debug --target docs-verify    # docgen --check + consistency
scripts/check_consistency.py --bin build/debug/tgbot # SSOT checks C1-C5
```

## Deployment

A hardened systemd unit and Dockerfile are added in M7. See
[`docs/deployment.md`](docs/deployment.md) for notes on io_uring and seccomp.

## Repository layout

```
include/tg/       public headers (tg_ module prefix)
src/              first-party sources; *.def SSOT tables; libtgbot_core + tgbot
tests/unit/       cmocka unit tests (test_*.c)
tests/integration/ end-to-end tests against the mock Telegram server
tests/fuzz/       libFuzzer harnesses (fuzz_*.c)
tests/mock/       scripted mock Bot API server + scenarios
tests/scripts/    unit tests for the Python check scripts
tests/golden/     golden files (e.g. --help output)
docs/             prose docs, Doxygen config, ADRs, man pages
config/           generated sample configuration
cmake/            Warnings/Sanitizers/Hardening/FindLiburing modules
third_party/      vendored cmocka + yyjson (pinned submodules)
scripts/          format/tidy/cppcheck/coverage/docgen/check_consistency
```

## Conventions

- C11, no GNU extensions in logic modules (allowed in `loop.c`/`tls.c`, documented).
- `snake_case` functions/vars, `module_verb_noun()`, types `_t`, macros/enums `UPPER_SNAKE`.
- Every fallible function returns `int` (0 = ok, negative errno); no silent failures.
- No `strcpy/strcat/sprintf/gets/atoi`. Ownership documented in header comments.
- Never disable a sanitizer or warning to make something pass.
- Commit messages follow Conventional Commits (`feat:`, `fix:`, `test:`, `chore:`).

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the full developer workflow.

## Security

Report vulnerabilities per [`SECURITY.md`](SECURITY.md). The bot token is read only
from the config file or environment and is redacted in all logs and dumps.

## License

GNU General Public License v3.0 only (GPL-3.0-only). See [`LICENSE`](LICENSE).

## Acknowledgements

Built on `liburing`, OpenSSL, SQLite, `yyjson`, and `cmocka`.
