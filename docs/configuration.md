# Configuration

Configuration is loaded from `config.toml` (or `--config <file>`), environment
variables, and built-in defaults. Precedence is **CLI flag > environment variable
> config file > default**. The schema lives in `src/config.def`; this file is
checked against it by `scripts/check_consistency.py` (C4).

## Summary

<!-- BEGIN GENERATED: config-table -->
| Key | Type | Default | Restart | Description |
|---|---|---|---|---|
| `bot_token` | CFG_STR | *(secret)* | yes | Telegram bot token from @BotFather (required) |
| `mode` | CFG_ENUM | polling | yes | Update source: polling or webhook |
| `webhook.listen` | CFG_STR | 127.0.0.1:8080 | yes | Webhook listen address behind a TLS-terminating reverse proxy |
| `webhook.secret` | CFG_STR | *(secret)* | yes | X-Telegram-Bot-Api-Secret-Token value verified on inbound webhook requests |
| `db.path` | CFG_STR | ./bot.db | yes | SQLite database file path |
| `sudo_ids` | CFG_I64_LIST | *(required)* | yes | Comma-separated Telegram user ids granted bot-level sudo |
| `api.pool_size` | CFG_INT | 4 | yes | Number of keep-alive TLS connections to the Bot API |
| `api.base` | CFG_STR | https://api.telegram.org | yes | Bot API base URL; a plain http:// base skips TLS for local or self-hosted deployments |
| `log.level` | CFG_ENUM | info | no | Log verbosity: error, warn, info or debug |
<!-- END GENERATED: config-table -->

## Reference

<!-- BEGIN GENERATED: config-reference -->
### `bot_token`

Telegram bot token from @BotFather (required).

- **Type:** CFG_STR
- **Environment variable:** `BOT_TOKEN`
- **Default:** *(secret)*
- **Restart required:** yes

### `mode`

Update source: polling or webhook.

- **Type:** CFG_ENUM
- **Environment variable:** `MODE`
- **Default:** polling
- **Restart required:** yes
- **Allowed values:** polling, webhook

### `webhook.listen`

Webhook listen address behind a TLS-terminating reverse proxy.

- **Type:** CFG_STR
- **Environment variable:** `WEBHOOK_LISTEN`
- **Default:** 127.0.0.1:8080
- **Restart required:** yes

### `webhook.secret`

X-Telegram-Bot-Api-Secret-Token value verified on inbound webhook requests.

- **Type:** CFG_STR
- **Environment variable:** `WEBHOOK_SECRET`
- **Default:** *(secret)*
- **Restart required:** yes

### `db.path`

SQLite database file path.

- **Type:** CFG_STR
- **Environment variable:** `DB_PATH`
- **Default:** ./bot.db
- **Restart required:** yes

### `sudo_ids`

Comma-separated Telegram user ids granted bot-level sudo.

- **Type:** CFG_I64_LIST
- **Environment variable:** `SUDO_IDS`
- **Default:** *(required, no default)*
- **Restart required:** yes

### `api.pool_size`

Number of keep-alive TLS connections to the Bot API.

- **Type:** CFG_INT
- **Environment variable:** `API_POOL_SIZE`
- **Default:** 4
- **Restart required:** yes

### `api.base`

Bot API base URL; a plain http:// base skips TLS for local or self-hosted deployments.

- **Type:** CFG_STR
- **Environment variable:** `API_BASE`
- **Default:** https://api.telegram.org
- **Restart required:** yes

### `log.level`

Log verbosity: error, warn, info or debug.

- **Type:** CFG_ENUM
- **Environment variable:** `LOG_LEVEL`
- **Default:** info
- **Restart required:** no
- **Allowed values:** error, warn, info, debug
<!-- END GENERATED: config-reference -->
