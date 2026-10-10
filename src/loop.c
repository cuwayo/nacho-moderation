/**
 * @file loop.c
 * @brief io_uring ring setup, SQE preparation, submission, and CQE dispatch.
 *
 * @ingroup loop
 */
#include "tg/loop.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* This file is the liburing boundary. include-cleaner cannot attribute
 * liburing's symbols (io_uring_sqe, IOSQE_IO_LINK, IORING_SETUP_*,
 * __kernel_timespec, io_uring_cqe, ...) to <liburing.h>, so it emits false
 * "no header providing" errors throughout; the include below is the correct and
 * only provider. Suppress that one check for the whole translation unit. */
// NOLINTBEGIN(misc-include-cleaner)
#include <liburing.h>

/** @brief The concrete io_uring loop (ring + bound vtable). */
struct loop {
    struct io_uring ring; /**< The io_uring instance. */
    loop_ops_t ops;       /**< Vtable bound to this loop. */
    unsigned last_submit; /**< SQEs submitted in the last iteration. */
    int ready;            /**< 1 once the ring is initialized. */
};

/* --- Real ring operations -------------------------------------------------- */

/**
 * @brief Prepare an OP_CONNECT SQE, chaining the linked deadline when flagged.
 *
 * @param[in] ctx Loop.
 * @param[in] op  Op context (its flags decide IOSQE_IO_LINK).
 * @param[in] fd  Socket fd.
 * @param[in] sa  Target address.
 * @param[in] len Address length.
 * @return 0 on success, -EAGAIN if no SQE is free.
 */
static int real_prep_connect(void *ctx, op_t *op, int fd, const struct sockaddr *sa, socklen_t len)
{
    loop_t *l = ctx;
    struct io_uring_sqe *sqe = io_uring_get_sqe(&l->ring);
    if (sqe == NULL) {
        return -EAGAIN;
    }
    io_uring_prep_connect(sqe, fd, sa, len);
    /* If a linked deadline follows, mark this SQE IOSQE_IO_LINK so the
     * IORING_OP_LINK_TIMEOUT prepared next is chained to it (SPEC section 4.3). */
    if ((op->flags & OPF_TIMEOUT_LINKED) != 0u) {
        sqe->flags |= IOSQE_IO_LINK;
    }
    io_uring_sqe_set_data(sqe, op);
    return 0;
}

/**
 * @brief Prepare an OP_RECV SQE.
 *
 * @param[in] ctx Loop.
 * @param[in] op  Op context.
 * @param[in] fd  Socket fd.
 * @param[in] buf Receive buffer.
 * @param[in] len Buffer capacity.
 * @return 0 on success, -EAGAIN if no SQE is free.
 */
static int real_prep_recv(void *ctx, op_t *op, int fd, void *buf, size_t len)
{
    loop_t *l = ctx;
    struct io_uring_sqe *sqe = io_uring_get_sqe(&l->ring);
    if (sqe == NULL) {
        return -EAGAIN;
    }
    io_uring_prep_recv(sqe, fd, buf, len, 0);
    io_uring_sqe_set_data(sqe, op);
    return 0;
}

/**
 * @brief Prepare an OP_SEND SQE (MSG_NOSIGNAL so a closed peer does not signal).
 *
 * @param[in] ctx Loop.
 * @param[in] op  Op context.
 * @param[in] fd  Socket fd.
 * @param[in] buf Bytes to send.
 * @param[in] len Count.
 * @return 0 on success, -EAGAIN if no SQE is free.
 */
static int real_prep_send(void *ctx, op_t *op, int fd, const void *buf, size_t len)
{
    loop_t *l = ctx;
    struct io_uring_sqe *sqe = io_uring_get_sqe(&l->ring);
    if (sqe == NULL) {
        return -EAGAIN;
    }
    io_uring_prep_send(sqe, fd, buf, len, MSG_NOSIGNAL);
    io_uring_sqe_set_data(sqe, op);
    return 0;
}

/**
 * @brief Fill @p op's embedded timespec from a millisecond count.
 *
 * @param[out] op Op whose timer field is written.
 * @param[in]  ms Milliseconds.
 */
static void fill_timespec(op_t *op, uint64_t ms)
{
    op->timer.tv_sec = (int64_t)(ms / 1000u);
    op->timer.tv_nsec = (int64_t)((ms % 1000u) * 1000000u);
}

/**
 * @brief Prepare a standalone OP_TIMEOUT SQE.
 *
 * @param[in] ctx Loop.
 * @param[in] op  Op context (holds the timespec the kernel reads asynchronously).
 * @param[in] ms  Timeout in milliseconds.
 * @return 0 on success, -EAGAIN if no SQE is free.
 */
static int real_prep_timeout(void *ctx, op_t *op, uint64_t ms)
{
    loop_t *l = ctx;
    struct io_uring_sqe *sqe = io_uring_get_sqe(&l->ring);
    if (sqe == NULL) {
        return -EAGAIN;
    }
    /* Store the timespec inside the op: the kernel reads it asynchronously, so a
     * stack-local would dangle. op_timespec_t is layout-compatible with
     * struct __kernel_timespec. */
    fill_timespec(op, ms);
    io_uring_prep_timeout(sqe, (struct __kernel_timespec *)&op->timer, 0, 0);
    io_uring_sqe_set_data(sqe, op);
    return 0;
}

/**
 * @brief Prepare a linked IORING_OP_LINK_TIMEOUT SQE.
 *
 * @param[in] ctx  Loop.
 * @param[in] link Op context receiving the timeout completion.
 * @param[in] ms   Timeout in milliseconds.
 * @return 0 on success, -EAGAIN if no SQE is free.
 */
static int real_prep_link_timeout(void *ctx, op_t *link, uint64_t ms)
{
    loop_t *l = ctx;
    struct io_uring_sqe *sqe = io_uring_get_sqe(&l->ring);
    if (sqe == NULL) {
        return -EAGAIN;
    }
    /* The linked timeout applies to the preceding SQE; its own completion is
     * reported through @p link (res == -ETIME). The timespec lives in the op. */
    fill_timespec(link, ms);
    io_uring_prep_link_timeout(sqe, (struct __kernel_timespec *)&link->timer, 0);
    io_uring_sqe_set_data(sqe, link);
    return 0;
}

/**
 * @brief Prepare an eventfd read (worker-thread handoff).
 *
 * @param[in] ctx Loop.
 * @param[in] op  Op context.
 * @param[in] efd eventfd to read.
 * @return 0 on success, -EAGAIN if no SQE is free.
 */
static int real_prep_read_eventfd(void *ctx, op_t *op, int efd)
{
    loop_t *l = ctx;
    struct io_uring_sqe *sqe = io_uring_get_sqe(&l->ring);
    if (sqe == NULL) {
        return -EAGAIN;
    }
    /* The loop owns a stable 8-byte counter buffer per eventfd read. */
    static _Thread_local uint64_t counter;
    io_uring_prep_read(sqe, efd, &counter, sizeof(counter), 0);
    io_uring_sqe_set_data(sqe, op);
    return 0;
}

/**
 * @brief Prepare an OP_CLOSE SQE.
 *
 * @param[in] ctx Loop.
 * @param[in] op  Op context.
 * @param[in] fd  Descriptor to close.
 * @return 0 on success, -EAGAIN if no SQE is free.
 */
static int real_prep_close(void *ctx, op_t *op, int fd)
{
    loop_t *l = ctx;
    struct io_uring_sqe *sqe = io_uring_get_sqe(&l->ring);
    if (sqe == NULL) {
        return -EAGAIN;
    }
    io_uring_prep_close(sqe, fd);
    io_uring_sqe_set_data(sqe, op);
    return 0;
}

/**
 * @brief Submit all queued SQEs (one submit per loop iteration).
 *
 * @param[in] ctx Loop.
 * @return Number submitted, or negative errno.
 */
static int real_submit(void *ctx)
{
    loop_t *l = ctx;
    int n = io_uring_submit(&l->ring);
    if (n < 0) {
        return n;
    }
    l->last_submit = (unsigned)n;
    return n;
}

/**
 * @brief Block for one CQE and return its op and result.
 *
 * @param[in]  ctx Loop.
 * @param[out] op  Receives the op pointer from the CQE's user_data.
 * @param[out] res Receives the completion result.
 * @return 0 on success, negative errno on failure.
 */
static int real_wait_cqe(void *ctx, op_t **op, int *res)
{
    loop_t *l = ctx;
    struct io_uring_cqe *cqe = NULL;
    int rc = io_uring_wait_cqe(&l->ring, &cqe);
    if (rc < 0) {
        return rc;
    }
    *op = io_uring_cqe_get_data(cqe);
    *res = cqe->res;
    io_uring_cqe_seen(&l->ring, cqe);
    return 0;
}

/* --- Public API ------------------------------------------------------------ */

loop_t *loop_create(unsigned entries)
{
    loop_t *l = calloc(1, sizeof(*l));
    if (l == NULL) {
        return NULL;
    }

    /* STATE: uninitialized -> ready on successful io_uring_queue_init.
     * Try the preferred flags first; fall back to defaults if the kernel
     * rejects them (SPEC section 4.3). */
    unsigned flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
    int rc = io_uring_queue_init(entries, &l->ring, flags);
    if (rc < 0) {
        rc = io_uring_queue_init(entries, &l->ring, 0);
    }
    if (rc < 0) {
        free(l);
        return NULL;
    }

    l->ops = (loop_ops_t){
        .prep_connect = real_prep_connect,
        .prep_recv = real_prep_recv,
        .prep_send = real_prep_send,
        .prep_timeout = real_prep_timeout,
        .prep_link_timeout = real_prep_link_timeout,
        .prep_read_eventfd = real_prep_read_eventfd,
        .prep_close = real_prep_close,
        .submit = real_submit,
        .wait_cqe = real_wait_cqe,
        .ctx = l,
    };
    l->ready = 1;
    return l;
}

const loop_ops_t *loop_ops(loop_t *l)
{
    return l == NULL ? NULL : &l->ops;
}

int loop_run_once(loop_t *l)
{
    if (l == NULL || !l->ready) {
        return -EINVAL;
    }
    /* One submit per iteration (SPEC section 4.3). */
    int rc = l->ops.submit(l);
    if (rc < 0 && rc != -EAGAIN) {
        return rc;
    }

    op_t *op = NULL;
    int res = 0;
    rc = l->ops.wait_cqe(l, &op, &res);
    if (rc == -EINTR) {
        return 0; /* interrupted by a signal; caller re-checks its stop flag */
    }
    if (rc < 0) {
        return rc;
    }
    if (op != NULL && op->on_complete != NULL) {
        op->on_complete(op, res);
    }
    return 0;
}

void loop_destroy(loop_t *l)
{
    if (l == NULL) {
        return;
    }
    if (l->ready) {
        io_uring_queue_exit(&l->ring);
        l->ready = 0;
    }
    free(l);
}

unsigned loop_last_submit_count(const loop_t *l)
{
    return l == NULL ? 0u : l->last_submit;
}

// NOLINTEND(misc-include-cleaner)
