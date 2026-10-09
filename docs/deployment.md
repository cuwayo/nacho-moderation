# Deployment

Deployment tooling (a hardened systemd unit and a Dockerfile) is added in M7. This
page documents the intended approach and the io_uring caveats.

## systemd

Install the binary and the sample config, then enable the unit:

```sh
sudo install -m 0755 build/release/tgbot /usr/local/bin/tgbot
sudo install -d -m 0700 /etc/nachomoderator
sudo install -m 0600 config/config.example.toml /etc/nachomoderator/config.toml
# edit /etc/nachomoderator/config.toml and set bot_token
sudo systemctl enable --now nachomoderator.service
```

The unit hardens the service with `NoNewPrivileges=yes`, `ProtectSystem=strict`,
`ProtectHome=yes`, `PrivateTmp=yes`, `MemoryDenyWriteExecute=yes`, a restricted
`SystemCallFilter`, and `Restart=on-failure`.

## Docker

> **Warning:** Docker's default seccomp profile may block the io_uring syscalls
> (`io_uring_setup`, `io_uring_enter`, `io_uring_register`). Run the container with
> `--security-opt seccomp=unconfined` or a custom profile that allows them, or run
> natively on the host.

## Webhook mode

In webhook mode the bot listens on plain HTTP behind a reverse proxy (nginx or
caddy) that terminates TLS. Set `webhook.listen` and `webhook.secret`; the proxy
must forward the `X-Telegram-Bot-Api-Secret-Token` header.
