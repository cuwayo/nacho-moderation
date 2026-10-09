# ADR 0003: SQLite in a dedicated worker thread

- **Status:** accepted
- **Date:** 2026-10-09
- **Deciders:** project maintainers

## Context

SQLite is a blocking, synchronous API. SPEC §4.4 requires it to be accessed only
from a dedicated DB worker thread, with the main loop notified of completions
through an `eventfd`.

## Decision

All SQLite access happens on a single DB worker thread. The loop submits
`db_job_t` jobs to an MPSC queue; the worker executes them with prepared
statements and pushes results to a response queue, then writes to the `eventfd`.
The loop reads the `eventfd` via io_uring and dispatches callbacks on the main
thread.

## Consequences

- The main thread never touches SQLite or its file descriptors.
- Statements are prepared once and reused; SQL is never string-concatenated.
- Hot data is cached in memory with a TTL to avoid round-trips.

## Alternatives considered

- **SQLite on the main thread:** rejected — blocking file I/O would stall the
  event loop and violate the non-blocking rule.
- **A thread pool of DB workers:** rejected — SQLite serializes writes anyway, and
  a single worker keeps ordering simple.
