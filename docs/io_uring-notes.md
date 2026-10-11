# io_uring notes

## Kernel requirements

- Minimum: Linux 5.15. Target: 6.1+.
- The ring is set up with `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN`
  when the kernel supports it, falling back to default flags otherwise.
- Feature availability is probed at runtime with `io_uring_get_probe()` /
  `io_uring_opcode_supported()`.

## Blocking on the main thread

The main thread must never issue blocking syscalls. All socket I/O, timers, and
`eventfd` reads go through the ring; DNS and SQLite run on worker threads.
`scripts/check_blocking.sh` enforces this with `strace`.

> **Warning:** Docker's default seccomp profile may block the io_uring syscalls.
> Run containers with `--security-opt seccomp=unconfined` or a custom profile, or
> run natively on the host.

## Op context

Every submitted SQE carries a pointer in `user_data` to an `op_t` (see
`docs/SPEC.md` §4.3). Completion handlers dispatch on `op->type`:

- `-EINTR` / `-EAGAIN`: resubmit the same operation.
- `-ECANCELED`: the linked timeout (or a cancellation) fired.
- Short reads/writes: advance the offset and resubmit the remainder.

## Batching

`io_uring_submit()` is called once per loop iteration, after all SQEs for that
iteration have been prepared.

## Timers

`IORING_OP_LINK_TIMEOUT` provides per-connection connect/read deadlines;
standalone timers use `IORING_OP_TIMEOUT`. No operation may block the loop for
more than 5 ms.

> **Implementation note:** a timeout's `timespec` is read by the kernel
> asynchronously, so it must outlive the submission. The `op_t` embeds an
> `op_timespec_t timer` field for this; never pass a stack-local `timespec` to
> `io_uring_prep_timeout` / `io_uring_prep_link_timeout`.

## M1 connection flow (state transitions)

```
CONN_IDLE --conn_connect--> CONN_CONNECTING
CONN_CONNECTING --OP_CONNECT=0 (+TLS)--> CONN_TLS_HANDSHAKE
CONN_CONNECTING --OP_CONNECT=0 (plain)--> CONN_HTTP_WRITE
CONN_TLS_HANDSHAKE --handshake done--> CONN_HTTP_WRITE
CONN_HTTP_WRITE --all bytes sent--> CONN_HTTP_READ
CONN_HTTP_READ --response complete--> CONN_DONE
any --error / -ETIME--> CONN_ERROR
```

`-EINTR`/`-EAGAIN` resubmit the same op without a state change; a short write
resubmits the remaining bytes; `-ECANCELED` on the linked timeout is the normal
cancellation when its companion completed and is ignored.

> **TLS request buffering:** `conn_send_request()` may be called while the
> connection is still `CONN_CONNECTING`/`CONN_TLS_HANDSHAKE` (the API pool does
> exactly this). `SSL_write()` cannot accept application data before the
> handshake, and OpenSSL retains nothing it refuses, so the plaintext is buffered
> in the connection and encrypted by `conn_pump()`/`conn_flush_request()` once the
> session is established. Encrypting early would silently drop the request and
> block the loop forever in `io_uring_wait_cqe`.

## M2 request flow (state transitions)

```
conn: CONN_DONE --conn_reuse_begin--> CONN_HTTP_WRITE   (same fd + TLS session)
      CONN_DONE --(Connection: close / leftover bytes)--> destroyed, reconnected
pool: REQ_QUEUED --dispatch (token + slot)--> REQ_INFLIGHT --completion--> REQ_COMPLETED
      REQ_COMPLETED --429/5xx backoff--> REQ_QUEUED --dispatch--> ...
      REQ_COMPLETED --terminal--> callback once, slot reused or dropped
poll: getUpdates issued --reply--> offset = max(update_id)+1 --re-arm--> getUpdates
```

The pool arms one `OP_TIMEOUT` for the earliest time a queued request could make
progress (next rate-limiter token or backoff expiry); there is no polling on the
main thread. The long-poll's own deadline is derived from `polling.timeout`, not
`api.request_timeout_ms`, so a 30 s poll is not aborted by a shorter request
timeout.

## M3 DB worker handoff (eventfd)

```
loop:   db_arm --prep OP_EVENTFD--> armed
worker: job done --push response queue--> write(eventfd)
loop:   OP_EVENTFD completion --db_pump (callbacks)--> db_arm (re-arm) --> armed
```

The loop submits one `OP_EVENTFD` read on the DB worker's eventfd and re-arms it
after every completion, so a completion that lands between the read finishing and
the re-arm is still observed (the eventfd counter accumulates). The loop thread
never touches SQLite: it only enqueues `db_job_t` values under a mutex and drains
the response queue in `db_pump()`. This is the same worker-thread pattern the DNS
resolver uses, and it is the only new io_uring consumer in M3.

## M7 signalfd delivery (SPEC section 8)

```
setup: sigprocmask(SIG_BLOCK) + signalfd() --signals_create--> fd
loop:  signals_start --prep_read(signalfd)--> armed
signal: SIGINT/SIGTERM/SIGUSR1 --kernel writes fd--> read CQE (on_signal_read)
       --callback sets the stop flag--> signals re-arms the read
```

A signal must wake `io_uring_wait_cqe`, which restarts itself on `EINTR`; an
asynchronous handler that only sets a flag never does. The signalfd read is
deliberately **not** `SFD_NONBLOCK`: the io_uring read must block in the kernel
until a signal arrives, otherwise it completes immediately with `-EAGAIN` and
busy-loops the event loop.

## M7 webhook accept/read/write

```
setup: bind + listen (startup, blocking-ok) --webhook_start--> prep_accept --> armed
peer:  connection --> OP_ACCEPT CQE (on_accept) --slot--> prep_recv --> WH_READ
data:  OP_RECV CQE --http_req_feed--> complete? verify secret -> route -> respond
done:  OP_SEND CQE (all bytes) --Connection: close--> slot released, accept re-armed
```

One accept is outstanding at all times while running. A request with the wrong
`X-Telegram-Bot-Api-Secret-Token` is answered 403 and a non-POST request 405,
without reaching the router.

## M7 graceful-shutdown drain (SPEC section 9)

```
SIGTERM --signalfd CQE--> g_stop --update source stops--> drain_in_flight
  loop: loop_run_once_timeout (bounded) --api_pump + db_pump--> until
        shared requests == 0 and DB jobs == 0, or the 10 s budget elapses
```

`loop_run_once_timeout` wraps `io_uring_wait_cqe_timeout`; the plain
`loop_run_once` waits indefinitely, so a pending sweep timer could otherwise block
the drain past its budget. The dedicated polling connection is excluded from the
drain (a long-poll is the update source, which shutdown stops rather than drains).
