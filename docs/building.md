# Building

## Prerequisites

- Linux ≥ 5.15 (target 6.1+).
- GCC ≥ 12 **and** Clang ≥ 16.
- CMake ≥ 3.25 + Ninja.
- `liburing-dev`, `libssl-dev`, `libsqlite3-dev`, `libpcre2-dev`.
- `clang-format`, `clang-tidy`, `cppcheck`, and (for coverage) `gcovr` or `lcov`.
- Doxygen ≥ 1.9.8 and Graphviz for the docs targets (optional).

```sh
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
    liburing-dev libssl-dev libsqlite3-dev libpcre2-dev \
    clang clang-format clang-tidy cppcheck gcovr doxygen graphviz
```

Vendored dependencies (`cmocka`, `yyjson`) are pinned git submodules; fetch them
with `git submodule update --init --recursive`.

## Presets

| Preset | Purpose |
|---|---|
| `debug` | `-O0 -g3`, warnings as errors |
| `asan` | `debug` + ASan/LSan/UBSan (the gating run) |
| `tsan` | `debug` + TSan (for the DB worker thread) |
| `coverage` | `debug` + `--coverage` |
| `fuzz` | Clang libFuzzer targets |
| `release` | `-O2 -g -DNDEBUG -flto` + hardening |

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Build trees are out-of-tree under `build/<preset>/`.

## The `tgbot` CLI

The binary's help output is reproduced verbatim below and checked against
`tests/golden/help.txt` (consistency check C8), so the docs cannot drift from the
binary:

```text
tgbot 0.0.1 - asynchronous Telegram group admin bot

Usage:
  tgbot [OPTIONS]

Options:
  --config <file>       Path to config.toml (default: ./config.toml)
  --check-config <file> Validate a config file and exit
  --log-level <level>   Override log level (error|warn|info|debug)
  -h, --help            Show this help and exit
  -V, --version         Show version and exit

Exit codes:
  0   success
  1   runtime or configuration error
  2   usage error
```

Hidden introspection flags (used by the doc/consistency scripts, not shown in
`--help`): `--dump-commands-json`, `--dump-config-json`, `--dump-tables-json`,
`--dump-config-example`.

## Sanitizers and coverage

```sh
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset coverage && cmake --build --preset coverage && ctest --preset coverage
cmake --build --preset coverage --target coverage
```

## Documentation targets

```sh
cmake --build --preset debug --target docs          # Doxygen HTML
cmake --build --preset debug --target docs-check    # Doxygen, warnings as errors
cmake --build --preset debug --target docs-update   # rewrite generated doc blocks
cmake --build --preset debug --target docs-verify   # docgen --check + consistency
```

When Doxygen is not installed, configuration still succeeds and the `docs` target
is skipped with a status message.
