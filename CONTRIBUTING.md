# Contributing

Thanks for contributing to nacho-moderator. The project is a Python 3.12 aiogram
bot managed with `uv`; see [`CLAUDE.md`](CLAUDE.md) for the rules and
[`docs/architecture.md`](docs/architecture.md) for the module map.

## Development environment

```sh
uv sync
uv run pytest
```

## Build and test

```sh
uv run ruff check .     # lint
uv run ruff format .    # format
uv run mypy src         # type-check (strict)
uv run pytest --cov     # tests + coverage (floor 85%)
```

## Definition of Done

Every change must:

- Keep `ruff check`, `mypy src`, and `pytest` green.
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
4. Update `docs/commands.md`.
5. Add a CHANGELOG `Unreleased` entry.

## Adding a config key

1. Add the field to the relevant model in `src/nacho_moderator/config.py` and, if
   it is settable from the environment, to `_ENV_MAP`.
2. Add validation if needed, and tests in `tests/unit/test_config.py`.
3. Update `docs/configuration.md` and `config/config.example.toml`.
4. Add a CHANGELOG entry.

## Code style

- Python 3.12, 4-space indent, 100-column lines (ruff enforces both).
- Type annotations everywhere; `mypy --strict` must pass.
- Module docstrings and docstrings on public functions and classes.
- Prefer small, single-purpose modules; keep handlers thin and push logic into
  services where it can be tested without a bot.
