/**
 * @file loop.h
 * @brief io_uring event loop: ring setup, SQE submission, CQE dispatch.
 *
 * Every submitted SQE carries an @ref op_t pointer in `user_data`; the loop
 * dispatches each CQE to `op->on_complete(op, res)` by @ref op_type_t. All
 * submission goes through the injectable @ref loop_ops_t so logic modules can be
 * tested with a fake ring (SPEC-ADDENDUM-TOOLCHAIN A3, A5.2).
 *
 * @ingroup loop
 */
#ifndef TG_LOOP_H
#define TG_LOOP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/** @brief Operation type carried by every op_t (SPEC section 4.3). */
typedef enum {
    OP_ACCEPT = 0,   /**< Inbound accept (webhook mode, M7). */
    OP_RECV,         /**< Receive on a socket. */
    OP_SEND,         /**< Send on a socket. */
    OP_CONNECT,      /**< Outbound connect. */
    OP_TIMEOUT,      /**< Standalone timer. */
    OP_LINK_TIMEOUT, /**< Linked timeout companion (IORING_OP_LINK_TIMEOUT). */
    OP_EVENTFD,      /**< Read a worker-thread eventfd (SPEC section 4.4). */
    OP_CLOSE         /**< Close a descriptor. */
} op_type_t;

/** @brief op_t flags. */
#define OPF_TIMEOUT_LINKED 0x01u /**< Mark the op IOSQE_IO_LINK so a link timeout follows. */

/** @brief Kernel timespec storage for timer ops.
 *
 * The kernel reads a timeout's timespec asynchronously, so it must outlive the
 * submission call. Embedding it in the op (which the caller owns for the op's
 * lifetime) avoids a stack-local that would dangle. Layout matches
 * `struct __kernel_timespec` (two 64-bit fields). */
typedef struct {
    int64_t tv_sec;  /**< Seconds. */
    int64_t tv_nsec; /**< Nanoseconds. */
} op_timespec_t;

/** @brief Context for one in-flight operation. */
typedef struct op {
    op_type_t type;                              /**< Operation type. */
    void *owner;                                 /**< Owning struct (conn_t*, ...). */
    void (*on_complete)(struct op *op, int res); /**< Completion callback. */
    int fd;                                      /**< Target fd (-1 if not applicable). */
    uint64_t deadline_ms;                        /**< Timer deadline (timers only). */
    uint32_t flags;                              /**< OPF_* flags. */
    op_timespec_t timer;                         /**< Timespec storage for OP_TIMEOUT. */
} op_t;

/** @brief Injectable ring interface; the real impl wraps liburing, fakes record. */
typedef struct {
    int (*prep_connect)(void *ctx, op_t *op, int fd, const struct sockaddr *sa, socklen_t len);
    int (*prep_recv)(void *ctx, op_t *op, int fd, void *buf, size_t len);
    int (*prep_send)(void *ctx, op_t *op, int fd, const void *buf, size_t len);
    int (*prep_timeout)(void *ctx, op_t *op, uint64_t ms);
    int (*prep_link_timeout)(void *ctx, op_t *link, uint64_t ms);
    int (*prep_read_eventfd)(void *ctx, op_t *op, int efd);
    int (*prep_close)(void *ctx, op_t *op, int fd);
    int (*submit)(void *ctx);                        /**< One submit per loop iteration. */
    int (*wait_cqe)(void *ctx, op_t **op, int *res); /**< Block for one CQE. */
    void *ctx;                                       /**< Implementation state. */
} loop_ops_t;

/** @brief The concrete io_uring loop. */
typedef struct loop loop_t;

/**
 * @brief Create and initialize the ring, probing for optional features.
 *
 * Requests `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN` and falls
 * back to default flags when the kernel rejects them (SPEC section 4.3).
 *
 * @param[in] entries Ring queue depth (rounded up to a power of two).
 *
 * @return A new loop, or NULL on failure.
 * @ownership Caller must call loop_destroy().
 * @thread_safety Not thread-safe (create on the loop thread).
 */
loop_t *loop_create(unsigned entries);

/**
 * @brief Return the injectable ops vtable for @p l.
 *
 * @param[in] l Loop.
 *
 * @return Pointer to the ops vtable (owned by the loop), or NULL.
 * @ownership Owned by the loop; do not free.
 * @thread_safety Read-only after loop_create().
 */
const loop_ops_t *loop_ops(loop_t *l);

/**
 * @brief Run one iteration: submit queued SQEs, then dispatch ready CQEs.
 *
 * Blocks until at least one CQE is available (or a signal interrupts the wait,
 * in which case it returns 0). Dispatches each CQE to its op's callback.
 *
 * @param[in] l Loop.
 *
 * @return 0 on success (including a spurious wakeup), negative errno on a ring
 *         error (-EINTR is swallowed).
 * @thread_safety Loop thread only.
 */
int loop_run_once(loop_t *l);

/**
 * @brief Destroy the ring and free the loop.
 *
 * @param[in] l Loop (may be NULL).
 * @thread_safety Loop thread only.
 */
void loop_destroy(loop_t *l);

/**
 * @brief Return the number of SQEs submitted in the most recent iteration.
 *
 * @param[in] l Loop.
 *
 * @return Submit count (for the batching assertion in tests).
 * @thread_safety Loop thread only.
 */
unsigned loop_last_submit_count(const loop_t *l);

#endif /* TG_LOOP_H */
