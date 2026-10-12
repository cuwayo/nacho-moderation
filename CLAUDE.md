# nacho-moderator

An asyncio Telegram admin/moderator bot written in Python 3.12, built on
[aiogram 3](https://docs.aiogram.dev) and managed with [uv](https://docs.astral.sh/uv/).

## Toolchain

- Python 3.12, dependencies and the virtualenv managed by **uv** (`uv sync`,
  `uv run ...`). Never use bare `pip`; never hand-edit `uv.lock`.
- Lint with **ruff**, type-check with **mypy** (strict), test with **pytest**.
- Exact, pinned versions live in `pyproject.toml`.

## Commands

```sh
uv sync                      # create/refresh the environment
uv run pytest                # tests
uv run pytest --cov          # tests + coverage (floor 85%)
uv run ruff check .          # lint
uv run ruff format .         # format
uv run mypy src              # type-check
uv run nacho-moderator --config config.toml
```

## Rules for agents

1. **Tests ship with the code.** Every new module or behaviour gets tests in the
   same change. Parsers and untrusted-input handling are test-first.
2. **Test this project's logic, not the libraries.** Use the doubles in
   `tests/conftest.py` (`FakeBot`, `FakeClock`, in-memory SQLite). Do not drive
   the aiogram dispatcher or the network from tests.
3. **Keep the environment green.** `ruff`, `mypy` (strict), and `pytest` must all
   pass before you finish. Never weaken a lint rule, a type, or a test to make
   something pass — fix the root cause, or ask.
4. **Inject time and randomness.** Logic never calls `time.time()`,
   `time.monotonic()`, or `secrets` directly; take a `Clock` / `RandomSource` so
   tests stay deterministic.
5. **Single source of truth for commands.** Every command is declared once in
   `src/nacho_moderator/commands/registry.py`; `/help` and `setMyCommands` are
   derived from it. Never add a command anywhere else.
6. **Secrets never leak.** The bot token and webhook secret are redacted from
   logs and from any diagnostic output.
7. **HTML-escape user text** wherever it is echoed into a message; only trusted
   markup (the `{mention}` placeholder) is sent unescaped.
8. **Conventional Commits** (`feat:`, `fix:`, `test:`, `chore:`, `docs:`).

## Layout

- `src/nacho_moderator/` — the bot package (see `docs/architecture.md` for the
  module map).
- `tests/` — pytest suite plus a standalone mock Bot API server.
- `docs/` — prose documentation.
- `packaging/` — systemd unit and Dockerfile.

## Documentation

Update `README.md`, `docs/`, and the `CHANGELOG.md` `Unreleased` section in the
same change as the code. A new command updates `docs/commands.md`; a new config
key updates `docs/configuration.md` and `config/config.example.toml`.
