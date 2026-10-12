# Troubleshooting

## The bot does not start: "bot_token is required"

Set `bot_token` in `config.toml`, or export `BOT_TOKEN`. Run
`nacho-moderator --check-config config.toml` to see the exact validation error.

## Webhook mode refuses to start

`webhook.secret` must be set and at least 16 characters. Generate one with
`openssl rand -hex 32` and set it in the `[webhook]` table.

## Webhook returns 403

The `X-Telegram-Bot-Api-Secret-Token` header from the reverse proxy does not
match `webhook.secret`. Check the proxy configuration and the secret value.

## "The bot lacks rights" replies

The bot must hold the required right in the chat (for example
`can_restrict_members` for `/ban`). Grant the bot the corresponding admin right.

## Join handling / captcha does not fire

Telegram only delivers `chat_member` updates while the bot is an administrator
in the chat. Promote the bot.

## HTTP 429 from the Bot API

The bot honours `retry_after` and pauses the affected bucket; other chats are
unaffected. Repeated 429s usually mean too many bulk operations per chat.

## `database is locked`

SQLite runs in WAL mode with a busy timeout. Persistent locks usually mean a
second process is using the same database file, or the DB path is on a network
filesystem (unsupported).

## High memory / slow responses

The rate limiter queues excess requests rather than dropping them; a large
backlog means the bot is generating calls faster than the limits allow. Check
`log.level = "debug"` output for retry and 429 messages.
