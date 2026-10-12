# Security

## Supported versions

The latest released minor version receives security fixes.

## Reporting a vulnerability

Please report suspected vulnerabilities privately (for example through GitHub's
private vulnerability reporting) rather than in a public issue. Include a
description, the affected version, and reproduction steps where possible.

## Scope

In scope:

- Leakage of the bot token or the webhook secret.
- Privilege escalation: a non-administrator executing an administrative command,
  or acting on a protected target (the owner, another administrator, or the bot).
- Forged webhook updates or captcha callback clicks.
- Parser bugs reachable from untrusted input (update JSON, callback data,
  blacklist patterns).
- Denial of service through a hostile blacklist regular expression.

## Hardening notes

- The token and webhook secret are redacted from logs and diagnostic output.
- Webhook updates are authenticated with a constant-time comparison of the
  `X-Telegram-Bot-Api-Secret-Token` header; a mismatch is rejected with 403, and
  the bot refuses to start in webhook mode without a secret of at least 16
  characters.
- Captcha and settings-panel callback payloads embed the owning user id and are
  rejected when pressed by anyone else.
- Every SQL query uses bound parameters; no SQL is built by string concatenation.
- Blacklist regular expressions are matched with a time limit so a hostile
  pattern cannot stall the event loop.
- The systemd unit ships a hardened profile; see
  [`docs/deployment.md`](docs/deployment.md).
