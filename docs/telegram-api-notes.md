# Telegram API notes

The set of Bot API methods used by the bot is defined once in
`src/api_methods.def`. This file is checked against it (consistency check C9 once
the mock server lands), and the mock server must implement every method listed.

## Methods used

| Method | Bucket | Idempotent |
|---|---|---|
| `getMe` | global | yes |
| `getUpdates` | global | yes |
| `getChatAdministrators` | global | yes |
| `sendMessage` | per-chat | no |
| `setMyCommands` | global | yes |

(Additional methods are added to `src/api_methods.def` as commands are implemented.)

## Rate limits

- Global: 25 requests/second.
- Per chat (messages): 15 requests/minute.
- Per chat (bulk operations): 1 request/second.

Excess requests are queued, never dropped silently. Buckets are token buckets
refilled from the injectable clock.

## Retry rules

- HTTP 429: read `parameters.retry_after`, pause the bucket, then retry.
- HTTP 5xx or network error: exponential backoff 1s, 2s, 4s, 8s, 16s, capped at
  30s, up to 5 retries for idempotent calls.
- HTTP 4xx other than 429: not retried; reported to the caller.

## Endpoints

- Default base: `https://api.telegram.org` (TLS via OpenSSL memory BIOs).
- A plain `http://` base (self-hosted Bot API server) skips TLS for local
  deployments, which also makes integration testing straightforward.
