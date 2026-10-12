# Deployment

The packaged systemd unit and Dockerfile live in [`packaging/`](../packaging/).

## Requirements

- Linux with Python 3.12 (the `uv` toolchain provisions it if needed).
- Outbound HTTPS to `api.telegram.org` (or your own Bot API base).
- For webhook mode: a reverse proxy that terminates TLS.

## Install with uv

```sh
uv sync
uv run nacho-moderator --config /etc/nachomoderator/config.toml
```

Or install the console script:

```sh
uv tool install .
nacho-moderator --config /etc/nachomoderator/config.toml
```

## systemd

Install the config and the unit, then enable it:

```sh
sudo useradd --system --home /var/lib/nachomoderator nachomoderator
sudo install -d -m 0700 /etc/nachomoderator
sudo install -m 0600 config/config.example.toml /etc/nachomoderator/config.toml
# edit /etc/nachomoderator/config.toml and set bot_token
sudo install -m 0644 packaging/nachomoderator.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now nachomoderator.service
```

The unit runs as the unprivileged `nachomoderator` user with a hardened profile
(`NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`,
`ReadWritePaths=/var/lib/nachomoderator` for the SQLite database, and
`Restart=on-failure`).

## Docker

```sh
docker build -f packaging/Dockerfile -t nacho-moderator .
docker run --rm \
    -v "$PWD/config.toml:/etc/nachomoderator/config.toml:ro" \
    -v nacho-data:/var/lib/nachomoderator \
    nacho-moderator
```

## Webhook mode

In webhook mode the bot listens on plain HTTP behind a reverse proxy that
terminates TLS. Set `mode = "webhook"`, `webhook.listen` and `webhook.secret`,
and point the proxy at the listen address. The proxy must forward the
`X-Telegram-Bot-Api-Secret-Token` header; the bot rejects a request whose token
does not match with HTTP 403, and a non-POST request with 405.

> **Required:** the secret must be set and at least 16 characters long; the bot
> refuses to start otherwise. Generate one with `openssl rand -hex 32`.

Register the public URL with Telegram out of band (for example with a
`setWebhook` call); the bot does not call `setWebhook` itself. Example nginx
location:

```nginx
location /webhook {
    proxy_pass http://127.0.0.1:8080;
    proxy_set_header X-Telegram-Bot-Api-Secret-Token "a-long-random-token";
}
```

## Notes

- The bot must be an administrator in a chat for `chat_member` updates (used for
  join handling and admin-cache invalidation) to be delivered.
- The SQLite database uses WAL mode; keep it on a local filesystem.
- Graceful shutdown drains in-flight updates for up to 10 seconds on
  `SIGINT`/`SIGTERM`.
