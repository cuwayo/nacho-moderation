# AGENTS.md

This project is an asyncio Telegram admin/moderator bot written in Python 3.12,
built on [aiogram 3](https://docs.aiogram.dev) and managed with
[uv](https://docs.astral.sh/uv/).

**Before writing any code, read and follow the guidance in
[`CLAUDE.md`](CLAUDE.md).** In short:

- Use `uv` for everything (`uv sync`, `uv run ...`); never bare `pip`.
- Keep `ruff` (lint), `mypy` (strict types), and `pytest` green — never weaken a
  check to make something pass.
- Tests ship with the code and test *this project's* logic, using the doubles in
  `tests/conftest.py`, not the aiogram dispatcher or the network.
- Inject time and randomness (never call `time.time()`/`secrets` in logic).
- Keep the command registry as the single source of truth for commands.
- Redact secrets; HTML-escape user text.
- Use Conventional Commits.

Further reading: [`docs/architecture.md`](docs/architecture.md),
[`docs/testing.md`](docs/testing.md), [`CONTRIBUTING.md`](CONTRIBUTING.md).
