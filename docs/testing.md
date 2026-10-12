# Testing

The test suite uses `pytest`. Tests exercise *this project's* logic — parsers,
services, handlers, the permission matrix, the rate limiter, and the automod
pipeline — against in-memory doubles and a fake bot that records the calls the
code makes. The suite never drives the aiogram library or the network.

```sh
uv sync
uv run pytest                 # all tests
uv run pytest -q              # quiet
uv run pytest --cov           # with a coverage report (floor 85%)
uv run pytest tests/unit/test_cmd_moderation.py -v
```

## Layout

```
tests/
  conftest.py        # fixtures: in-memory db, fake clock, FakeBot, fake admins
  helpers.py         # builders for messages and command contexts
  mock/              # a standalone mock Telegram Bot API server (for manual runs)
  unit/              # one file per module / behaviour area
```

## Test doubles

- **`FakeBot`** records every call the code makes (`send_message`,
  `ban_chat_member`, ...) and returns small canned values, so assertions are
  about our decisions, not about HTTP.
- **`FakeClock`** makes TTLs, flood windows, and captcha deadlines deterministic.
- **In-memory SQLite** (`Database.connect(":memory:")`) keeps DB tests hermetic.

## What is covered

| Area | File |
|---|---|
| Duration parsing, HTML escaping | `test_models.py` |
| Token buckets, retry policy, backoff | `test_transport.py` |
| Rate-limit/retry session middleware | `test_ratelimit_middleware.py` |
| Command parsing and target resolution | `test_router.py` |
| Permission matrix | `test_perms.py` |
| Administrator cache TTL/invalidation | `test_admin_cache.py` |
| Database schema, warns, actions, captcha | `test_db.py` |
| Automod (locks/blacklist/antiflood) | `test_automod.py` |
| Captcha service | `test_captcha_service.py` |
| Expiry sweeper | `test_expiry.py` |
| Command handlers | `test_cmd_*.py` |
| Dispatch, guards, automod middleware | `test_dispatch.py` |
| Join and callback handlers | `test_join_callback.py` |
| Config loading and precedence | `test_config.py` |
| CLI entry point | `test_cli.py` |

## Mock Bot API server (manual / soak)

`tests/mock/mock_tg_server.py` is a standalone plain-HTTP server that speaks
enough of the Bot API to run the real bot without a token or TLS. Point the bot
at it with `API_BASE=http://127.0.0.1:<port>`.

```sh
python3 tests/mock/mock_tg_server.py --port 8081 --scenario tests/mock/scenarios/ban_reply.json --log calls.jsonl
MODE=polling API_BASE=http://127.0.0.1:8081 BOT_TOKEN=123456:TEST uv run nacho-moderator
```

It supports scripted scenarios (`tests/mock/scenarios/*.json`) and fault
injection (429 with `retry_after`, 5xx, malformed JSON, connection reset), and
records every received call as JSONL.

## Coverage

`uv run pytest --cov` reports line coverage and fails below the floor configured
in `pyproject.toml` (85%).
