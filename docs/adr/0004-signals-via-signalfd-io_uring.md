# ADR 0004: Signals via signalfd on the io_uring ring

- **Status:** accepted
- **Date:** 2026-10-11
- **Deciders:** project maintainers

## Context

SPEC §8 requires signal handling to be done with `signalfd` driven by io_uring:
"`main.c` — init, signal handling (signalfd via io_uring), shutdown". SPEC §9
requires a graceful shutdown on `SIGINT`/`SIGTERM`: stop accepting updates, drain
in-flight requests (max 10 s), flush the database, close the ring, exit 0.

An earlier implementation installed asynchronous handlers with `sigaction()` that
only set a `volatile sig_atomic_t` flag. This does not wake the loop: `liburing`'s
`io_uring_wait_cqe()` restarts itself on `EINTR` internally, so a signal delivered
while the loop is blocked in the wait does not return control to the loop, and the
flag is not observed until the next completion arrives (for polling mode, up to the
30 s long-poll timeout). The process therefore did not stop promptly — and in
tests did not stop at all within the expected budget.

## Decision

We will block `SIGINT`, `SIGTERM` and `SIGUSR1` with `sigprocmask()` and read them
from a `signalfd(2)`. The read is submitted through the io_uring ring
(`src/signals.c`, `prep_read`), so an arriving signal produces a CQE that wakes
`io_uring_wait_cqe()` and dispatches the notification on the loop thread. The read
is re-armed after every delivery. The signalfd is deliberately **not**
`SFD_NONBLOCK`: the io_uring read must block in the kernel until a signal arrives,
otherwise it completes immediately with `-EAGAIN` and busy-loops the event loop.

The graceful-shutdown drain uses a bounded wait (`loop_run_once_timeout`, wrapping
`io_uring_wait_cqe_timeout`) so a pending sweep timer cannot block the loop past
the 10 s drain budget.

## Consequences

- A signal is observed promptly (within one loop iteration), and shutdown is
  bounded by the drain budget rather than by the longest outstanding timer.
- The main thread still performs no blocking syscall: the signalfd read is an
  io_uring operation, so `check_blocking.sh` remains clean.
- Signal disposition is process-wide (the mask is set at startup), so any future
  code that spawns threads inherits the mask; worker threads must not expect
  these signals.
- `SIGUSR1` metrics dumps now also run on the loop thread via the same path.

## Alternatives considered

- **Async handler setting a flag (previous approach):** rejected — does not wake
  `io_uring_wait_cqe` (it restarts on `EINTR`), so shutdown was delayed until the
  next completion and could exceed the budget.
- **Self-pipe (`socketpair`/`pipe`) written from an async handler:** would work and
  also wake the ring, but the spec names `signalfd` explicitly, and `signalfd`
  avoids the extra descriptor pair and the async-signal-safe `write` requirement.
- **`signalfd` with `SFD_NONBLOCK`:** rejected — an io_uring read on a non-blocking
  signalfd returns `-EAGAIN` immediately and busy-loops the loop.
