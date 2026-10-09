# Security Policy

## Supported versions

The project is pre-1.0; only the latest `main` is supported for security fixes.

## Reporting a vulnerability

Please report suspected vulnerabilities privately, **not** in a public issue. Use
GitHub's private vulnerability reporting (Security → Report a vulnerability) or
email the maintainers. Include a description, reproduction steps, and impact.

We aim to acknowledge reports within 72 hours and to provide a remediation plan
within 14 days.

## Scope

In scope:

- Leakage of the bot token (logs, dumps, error messages, URLs).
- Privilege escalation through commands (a non-admin executing admin commands).
- Parser bugs in HTTP/JSON/command parsing (memory safety, ReDoS).
- Forged callback data (captcha button ownership checks).

Out of scope:

- Vulnerabilities in `liburing`, OpenSSL, SQLite, or `yyjson` themselves (report
  upstream).
- Issues requiring a compromised host or a modified configuration.

## Hardening notes

- The bot token is read only from the config file (mode 0600 recommended) or the
  environment, and is redacted as `bot<token>` everywhere it could be logged or
  dumped.
- Webhook mode verifies the `X-Telegram-Bot-Api-Secret-Token` header and rejects
  mismatches with 403.
- All SQL uses prepared statements; user text sent to Telegram is HTML-escaped.
- The systemd unit (M7) restricts address families and system calls, explicitly
  allowing only the io_uring syscalls the bot needs.
