# ADR 0002: TLS via OpenSSL memory BIOs

- **Status:** accepted
- **Date:** 2026-10-09
- **Deciders:** project maintainers

## Context

The Bot API is reached over TLS. SPEC §3 requires OpenSSL 3 with **memory BIOs**
(`BIO_s_mem`) driven manually by io_uring `recv`/`send`; blocking `SSL_read` /
`SSL_write` on the socket fd is forbidden because it would block the event loop.

## Decision

Each TLS connection owns an `SSL` object with both the read and write BIOs set to
`BIO_s_mem`. Ciphertext is moved explicitly: io_uring `recv` results are fed to
`BIO_write(rbio)`, and pending `wbio` data is drained with `BIO_read` and sent via
io_uring `send`. The handshake is advanced by `SSL_do_handshake`, retried on
`SSL_ERROR_WANT_READ`/`SSL_ERROR_WANT_WRITE`.

## Consequences

- TLS never blocks the loop.
- The TLS adapter must handle partial records and resubmission of short writes.
- The pump logic is safety-critical and gets a state diagram and explicit
  `/* STATE: ... */` comments.

## Alternatives considered

- **Socket BIOs with a non-blocking fd:** rejected — OpenSSL would still perform
  its own I/O on the fd, bypassing io_uring and risking blocking behavior.
- **kTLS:** rejected — not portable across the required kernels and cipher suites.
