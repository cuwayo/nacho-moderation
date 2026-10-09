# ADR 0001: Single-threaded io_uring event loop

- **Status:** accepted
- **Date:** 2026-10-09
- **Deciders:** project maintainers

## Context

The bot performs a high volume of small network operations (long-poll
`getUpdates`, API calls, timers). SPEC §1 and §4 require a single-threaded event
loop built on `liburing`, with blocking work isolated in worker threads that
signal the loop through an `eventfd`.

## Decision

All network I/O runs asynchronously through `liburing` in a single-threaded loop
on the main thread. There is **no** `epoll`/`poll` fallback. The ring is set up
with `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN` when supported,
falling back to default flags. Blocking work (SQLite, DNS) runs on worker threads
and signals the loop via an `eventfd`.

## Consequences

- Handlers never block; multi-step flows use continuation callbacks.
- No locking is needed on loop-owned state.
- The main thread must not call blocking syscalls, enforced by
  `scripts/check_blocking.sh`.
- Requires a kernel with io_uring (≥ 5.15); Docker's seccomp profile may need
  relaxing.

## Alternatives considered

- **epoll + threads:** rejected — the spec forbids an epoll/poll fallback.
- **One thread per connection:** rejected — poor scalability and complex
  synchronization for no benefit at this workload.
