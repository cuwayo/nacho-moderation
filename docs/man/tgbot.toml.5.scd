tgbot.toml(5)

# NAME

tgbot.toml - configuration file for tgbot(1)

# DESCRIPTION

The configuration file uses TOML. Keys may be given as top-level entries or under
tables (for example *api.pool_size* or the *[api]* table with a *pool_size* key).
Precedence is: command-line flag, then environment variable, then this file, then
the built-in default.

The complete key reference is generated from the configuration schema.

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

# EXAMPLE

```
bot_token = "123456:REPLACE_ME"
mode = "polling"

[api]
pool_size = 4
base = "https://api.telegram.org"

[log]
level = "info"
```

# SEE ALSO

*tgbot*(1)
