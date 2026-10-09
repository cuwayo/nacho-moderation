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
