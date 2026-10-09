# nacho-moderator documentation

This is the landing page for the project documentation. Start with the
[README](../README.md) for a quick overview, then dive into the topics below.

## Guides

- [Architecture](architecture.md) — event loop, threads, data flow, and error policy.
- [Building](building.md) — prerequisites, presets, sanitizers, and troubleshooting.
- [Configuration](configuration.md) — every configuration key.
- [Commands](commands.md) — every bot command.
- [Testing](testing.md) — unit, integration, fuzz, soak, coverage, and consistency.
- [io_uring notes](io_uring-notes.md) — kernel requirements, probing, and pitfalls.
- [Telegram API notes](telegram-api-notes.md) — rate limits, retry rules, endpoints.
- [Deployment](deployment.md) — systemd, Docker, reverse proxy, seccomp.
- [Troubleshooting](troubleshooting.md) — FAQ and common errors.

## Specifications

The four binding specification documents live in this directory:

- [`SPEC.md`](SPEC.md)
- [`SPEC-ADDENDUM-TOOLCHAIN.md`](SPEC-ADDENDUM-TOOLCHAIN.md)
- [`SPEC-ADDENDUM-DOCS.md`](SPEC-ADDENDUM-DOCS.md)
- [`SPEC-ADDENDUM-SSOT.md`](SPEC-ADDENDUM-SSOT.md)

## Decisions

Architecture Decision Records live in [`adr/`](adr/).

## Terminology

Terms used consistently throughout the project are listed in
[`terminology.txt`](terminology.txt): "chat" vs "group" vs "supergroup",
"restrict" vs "mute", and similar.
