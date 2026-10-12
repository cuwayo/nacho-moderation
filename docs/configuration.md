# Configuration

Configuration comes from four sources. Precedence, highest first:

1. CLI flags (`--log-level`).
2. Environment variables.
3. The `config.toml` file (`--config <file>`, default `./config.toml`).
4. Built-in defaults.

A missing default `config.toml` is not an error; a file requested explicitly with
`--config` (or `--check-config`) that does not exist is. Unknown keys are
rejected.

## Keys

| Key | Env var | Type | Default | Description |
|---|---|---|---|---|
| `bot_token` | `BOT_TOKEN` | string | *(required)* | Telegram bot token issued by BotFather. Never logged. |
| `mode` | `MODE` | enum | `polling` | Update source: `polling` or `webhook`. |
| `sudo_ids` | `SUDO_IDS` | int list | *(empty)* | Comma-separated user ids granted bot-level sudo. |
| `webhook.listen` | `WEBHOOK_LISTEN` | string | `127.0.0.1:8080` | Webhook listen address behind a reverse proxy. |
| `webhook.secret` | `WEBHOOK_SECRET` | string | *(required in webhook mode)* | `X-Telegram-Bot-Api-Secret-Token` value; at least 16 characters. |
| `db.path` | `DB_PATH` | string | `./bot.db` | SQLite database file path. |
| `api.pool_size` | `API_POOL_SIZE` | int | `4` | Keep-alive connections to the Bot API. |
| `api.base` | `API_BASE` | string | `https://api.telegram.org` | Bot API base URL; a plain `http://` base skips TLS. |
| `api.request_timeout_ms` | `API_REQUEST_TIMEOUT_MS` | int | `15000` | Per-request deadline, in milliseconds. |
| `polling.timeout` | `POLLING_TIMEOUT` | int | `30` | `getUpdates` long-poll timeout, in seconds. |
| `log.level` | `LOG_LEVEL` | enum | `info` | `error`, `warn`, `info`, or `debug`. |

## Validation

- `bot_token` is required in every mode.
- `webhook.secret` must be set and at least 16 characters in webhook mode; the
  bot refuses to start otherwise.
- `api.pool_size` must be at least 1, `polling.timeout` non-negative, and
  `api.request_timeout_ms` positive.

`nacho-moderator --check-config <file>` validates a file without starting the
bot and exits non-zero on any problem.

## Example

```toml
bot_token = "123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11"
mode = "polling"
sudo_ids = "11111111,22222222"

[api]
pool_size = 4
base = "https://api.telegram.org"
request_timeout_ms = 15000

[polling]
timeout = 30

[log]
level = "info"
```

> **Note:** bare keys such as `mode` must appear **above** the first `[table]`
> header in a TOML file.

## Webhook mode

```toml
mode = "webhook"

[webhook]
listen = "127.0.0.1:8080"
secret = "a-long-random-token-at-least-16-chars"
```

The secret authenticates inbound updates; without it anyone reaching the port
could forge them. Generate one with `openssl rand -hex 32`.
