# Contributing

Thanks for contributing to nacho-moderator. The project is a Python 3.12 aiogram
bot managed with `uv`; see [`CLAUDE.md`](CLAUDE.md) for the rules and
[`docs/architecture.md`](docs/architecture.md) for the module map.

## Development environment

```sh
uv sync                     # create/refresh the environment
uv run pre-commit install   # optional: run the hooks on every commit
```

## Build and test

```sh
uv run ruff check .          # lint
uv run ruff format .         # format
uv run mypy                  # type-check (strict, src + tests + scripts)
uv run pytest                # tests
uv run pytest --cov          # tests + coverage (global floor 90%)
uv run python scripts/check_coverage.py coverage.json   # per-module floors
```

Run the suite the way CI does (asyncio debug, warnings as errors):

```sh
PYTHONASYNCIODEBUG=1 uv run python -X dev -W error -m pytest -q
```

## Toolchain

| Concern | Tool |
| --- | --- |
| Env/deps | `uv`; `uv sync --frozen`; `uv lock --check` in CI; Python pinned in `.python-version`. |
| Lint/format | `ruff`, with `E,W,F,I,N,UP,B,C4,SIM,RUF,ASYNC,S,D,ANN,PT,PL,PERF,TRY,ARG,ERA,T20,LOG,G,PTH,FBT` (Google docstrings). Every `ignore` in `pyproject.toml` carries a justification comment. |
| Types | `mypy --strict` over `src`, `tests`, and `scripts`; `warn_unused_ignores` stays on. |
| Tests | `pytest`, `pytest-asyncio` (auto), `pytest-cov`, `pytest-timeout` (30 s), `pytest-randomly`, optional `pytest-xdist`, `hypothesis`. The injectable clock is used instead of `freezegun`. |
| Coverage | `--cov-branch`; global floor 90%; per-module floors enforced by `scripts/check_coverage.py`. |
| Security | `pip-audit`, ruff `S` rules (bandit), `detect-secrets` against `.secrets.baseline`. |
| asyncio hygiene | ruff `ASYNC`; every `create_task` is tracked and stopped at shutdown; cancellation is never swallowed. |
| Property/fuzz | `hypothesis` for the parsers, HTML escaping, log redaction, the listen parser, the regex guard, and the token bucket. |
| Pre-commit | `.pre-commit-config.yaml`: ruff, ruff-format, mypy, detect-secrets, codespell, end-of-file/trailing-whitespace, `uv lock --check`. |
| Docs | markdownlint + lychee (`docs-lint`), README `title="ci"` snippets (`docs-smoke`), `tests/test_docs_consistency.py` (`docs-consistency`), `pdoc` (`docs-build`). |

## Definition of Done

Every change must:

- Keep `ruff check`, `ruff format --check`, `mypy`, and `pytest` green.
- Add tests for new modules and behaviours (test-first for parsers).
- Inject time and randomness; never call `time.time()`/`secrets` in logic.
- Keep the command registry the single source of truth for commands.
- Redact secrets and HTML-escape user text.
- Update the README/docs and the `CHANGELOG.md` `Unreleased` section.
- Use Conventional Commits (`feat:`, `fix:`, `test:`, `chore:`, `docs:`).

## Adding a command

1. Add one `CommandSpec` row to `src/nacho_moderator/commands/registry.py`.
2. Implement the handler in the appropriate `commands/*.py` module.
3. Add tests: the permission matrix row, argument parsing, and the behaviour.
4. Add a `### /<name>` heading with Examples and Edge cases to `docs/commands.md`
   (checked by `tests/test_docs_consistency.py`).
5. Add a CHANGELOG `Unreleased` entry.

## Adding a config key

1. Add the field to the relevant model in `src/nacho_moderator/config.py` and, if
   it is settable from the environment, to `_ENV_MAP`.
2. Add validation if needed, and tests in `tests/unit/test_config.py`.
3. Update `docs/configuration.md` and `config/config.example.toml` (checked by
   `tests/test_docs_consistency.py`).
4. Add a CHANGELOG entry.

## Code style

- Python 3.12, 4-space indent, 100-column lines (ruff enforces both).
- Type annotations everywhere; `mypy --strict` must pass.
- Google-style docstrings on public modules, classes, and functions; document
  Args, Returns, and Raises. Note loop affinity where it matters ("must be
  awaited on the event loop").
- Prefer small, single-purpose modules; keep handlers thin and push logic into
  services where it can be tested without a bot.
