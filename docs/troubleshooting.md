# Troubleshooting

## `io_uring_setup` fails with `EPERM` / `ENOSYS`

The kernel may have io_uring disabled, or a seccomp profile may be blocking it.
Check `sysctl kernel.io_uring_disabled`; in containers, run with
`--security-opt seccomp=unconfined`. See [`io_uring-notes.md`](io_uring-notes.md).

## TLS certificate errors

The bot verifies the Bot API hostname. A failure usually means an outdated CA
bundle or a proxy intercepting TLS. Update `ca-certificates` or point
`api.base` at a trusted endpoint.

## "The bot lacks rights" replies

The bot must hold the required right in the chat (for example
`can_restrict_members` for `/ban`). Grant the bot the corresponding admin right.

## HTTP 429 from the Bot API

The bot honours `retry_after` and pauses the affected bucket; other chats are
unaffected. Repeated 429s usually mean too many bulk operations per chat.

## `database is locked`

SQLite runs in WAL mode with a busy timeout. Persistent locks usually mean a
second process is using the same database file, or the DB path is on a network
filesystem (unsupported).
