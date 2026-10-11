# Deployment

The packaged systemd unit and Dockerfile live in [`packaging/`](../packaging/).
This page covers the io_uring and seccomp caveats and the webhook setup.

## systemd

Install the binary and the sample config, then enable the unit:

```sh
sudo install -m 0755 build/release/tgbot /usr/local/bin/tgbot
sudo useradd --system --home /var/lib/nachomoderator nachomoderator
sudo install -d -m 0700 /etc/nachomoderator
sudo install -m 0600 config/config.example.toml /etc/nachomoderator/config.toml
# edit /etc/nachomoderator/config.toml and set bot_token
sudo install -m 0644 packaging/nachomoderator.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now nachomoderator.service
```

The unit (`packaging/nachomoderator.service`) hardens the service with
`NoNewPrivileges=yes`, `ProtectSystem=strict`, `ProtectHome=yes`, `PrivateTmp=yes`,
`MemoryDenyWriteExecute=yes`, a restricted `SystemCallFilter` (the `@system-service`
set, which includes the io_uring syscalls), `ReadWritePaths=/var/lib/nachomoderator`
for the SQLite database, and `Restart=on-failure`. It runs as the unprivileged
`nachomoderator` user.

## Docker

```sh
docker build -f packaging/Dockerfile -t nacho-moderator .
docker run --rm --security-opt seccomp=unconfined \
    -v "$PWD/config.toml:/etc/nachomoderator/config.toml:ro" \
    -v nacho-data:/var/lib/nachomoderator \
    nacho-moderator
```

> **Warning:** Docker's default seccomp profile may block the io_uring syscalls
> (`io_uring_setup`, `io_uring_enter`, `io_uring_register`). Run the container with
> `--security-opt seccomp=unconfined` or a custom profile that allows them, or run
> natively on the host. The kernel must be >= 5.15.

## Webhook mode

In webhook mode the bot listens on plain HTTP behind a reverse proxy (nginx or
caddy) that terminates TLS. Set `mode = "webhook"`, `webhook.listen` and
`webhook.secret`, and point the proxy at the listen address. The proxy must
forward the `X-Telegram-Bot-Api-Secret-Token` header; the bot rejects a request
whose token does not match `webhook.secret` with HTTP 403 (and a non-POST request
with 405).

> **Required:** the secret must be set and at least 16 characters long. The bot
> **refuses to start** in webhook mode otherwise (and `--check-config` reports the
> same error). Without a secret, anyone who can reach the port could forge updates
> and impersonate administrators. Generate one with `openssl rand -hex 32`.

Register the public URL with Telegram out of band (for example with a
`setWebhook` call using your own tooling); the bot does not call `setWebhook`
itself. Example nginx location:

```nginx
location /webhook {
    proxy_pass http://127.0.0.1:8080;
    proxy_set_header X-Telegram-Bot-Api-Secret-Token "a-long-random-token";
}
```

## Graceful shutdown

On `SIGINT`/`SIGTERM` the bot stops accepting updates, drains in-flight Bot API
requests and DB jobs (bounded by 10 seconds), flushes the database, and exits 0.
`SIGUSR1` dumps the runtime counters to the log.
