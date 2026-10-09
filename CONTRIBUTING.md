# Contributing

Thanks for contributing to nacho-moderator. This project is governed by four
binding specifications; **read them before writing code**:

- [`docs/SPEC.md`](docs/SPEC.md)
- [`docs/SPEC-ADDENDUM-TOOLCHAIN.md`](docs/SPEC-ADDENDUM-TOOLCHAIN.md)
- [`docs/SPEC-ADDENDUM-DOCS.md`](docs/SPEC-ADDENDUM-DOCS.md)
- [`docs/SPEC-ADDENDUM-SSOT.md`](docs/SPEC-ADDENDUM-SSOT.md)

## Development environment

```sh
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
    liburing-dev libssl-dev libsqlite3-dev libpcre2-dev \
    clang clang-format clang-tidy cppcheck gcovr
git submodule update --init --recursive
```

## Build and test

```sh
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --output-on-failure
cmake --preset asan && cmake --build --preset asan
ctest --preset asan --output-on-failure
```

## Definition of Done

Every milestone or PR must satisfy both the toolchain DoD (A9) and the
documentation DoD (D9):

- Builds warning-free with GCC **and** Clang (`-Werror`).
- `clang-format`, `clang-tidy`, `cppcheck` clean.
- New code has unit tests; parsers/untrusted input have a fuzz target.
- `ctest` green under `asan`; `tsan` green if threads are touched.
- Coverage thresholds met.
- No new blocking syscall on the main thread (`scripts/check_blocking.sh`).
- README/docs updated; Doxygen comments written with the code.
- `docs-check` passes with zero warnings.
- `scripts/check_consistency.py` passes.
- CHANGELOG `Unreleased` updated.
- Conventional Commits (`feat:`, `fix:`, `test:`, `chore:`, `docs:`).

## Adding a command (S8 checklist)

1. Add one `X(...)` row to `src/commands.def`.
2. Create `src/cmd_<group>.c` with `int cmd_<name>(cmd_ctx_t *ctx)` and full
   Doxygen comments.
3. Add unit tests (permission matrix row, argument parsing) and, once the mock
   server exists, an integration scenario in `tests/mock/scenarios/`.
4. Create `docs/commands/<name>.md` with non-empty `Examples` and `Edge cases`.
5. Run `cmake --build --preset debug --target docs-update`.
6. Run `ctest --preset asan` and `scripts/check_consistency.py --bin build/debug/tgbot`.
7. Add a CHANGELOG `Unreleased` entry.

## Adding a config key

1. Add one `K(...)` row to `src/config.def` (and a `CFG_REQUIRED(...)` entry if it
   has no usable default).
2. If it is an enum, add `E(key, value)` rows to `src/config_enums.def`.
3. Add validator tests and a `docs-update` run.
4. Regenerate the sample config: `cmake --build --preset debug --target gen-config-example`.
5. Add a CHANGELOG entry.

## Code style

- C11, `snake_case`, `module_verb_noun()`, types `_t`, macros/enums `UPPER_SNAKE`.
- Every fallible function returns `int` (0 = ok, negative errno).
- Ownership documented in header comments; no `strcpy/strcat/sprintf/gets/atoi`.
- Never disable a sanitizer or warning to make something pass; fix the root cause.
- Every io_uring state transition is commented `/* STATE: X -> Y on event Z */`.

Formatting and static analysis configuration lives in `.clang-format`,
`.clang-tidy`, and `.pre-commit-config.yaml`.
