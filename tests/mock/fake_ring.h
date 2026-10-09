/**
 * @file fake_ring.h
 * @brief A loop_ops_t test double that records submitted ops and replays CQEs.
 *
 * Implements the @ref loop_ops_t interface without liburing so the connection
 * state machine and API client can be tested with no ring, no network, and no
 * real clock (SPEC-ADDENDUM-TOOLCHAIN A5.2). Tests prepare ops, call submit(),
 * then feed synthetic completions (including -EINTR, -ECANCELED, short reads and
 * writes) via fake_ring_complete*().
 *
 * @ingroup loop
 */
#ifndef TG_TEST_FAKE_RING_H
#define TG_TEST_FAKE_RING_H

#include <stddef.h>

#include "tg/loop.h"

/** @brief Maximum number of queued SQEs the fake ring tracks. */
#define FAKE_RING_CAPACITY 64u

/** @brief One recorded submission. */
typedef struct {
    op_type_t type;  /**< Operation type. */
    op_t *op;        /**< The op passed by the caller. */
    int fd;          /**< Target fd. */
    const void *buf; /**< Buffer pointer (recv/send). */
    size_t len;      /**< Buffer length. */
} fake_sqe_t;

/** @brief Fake ring state. */
typedef struct {
    fake_sqe_t pending[FAKE_RING_CAPACITY]; /**< Prepared but not yet submitted. */
    size_t pending_len;
    fake_sqe_t inflight[FAKE_RING_CAPACITY]; /**< Submitted, awaiting completion. */
    size_t inflight_len;
    loop_ops_t ops; /**< Vtable bound to this fake. */
} fake_ring_t;

/**
 * @brief Initialize the fake ring and return its vtable.
 *
 * @param[out] fr Fake ring state (caller-owned; no heap use).
 *
 * @return Pointer to @p fr's loop_ops_t.
 * @ownership @p fr must outlive the returned vtable.
 */
const loop_ops_t *fake_ring_init(fake_ring_t *fr);

/**
 * @brief Complete the oldest in-flight op with @p res.
 *
 * Pops the oldest in-flight entry and invokes its on_complete callback.
 *
 * @param[in,out] fr  Fake ring.
 * @param[in]     res Result to deliver.
 *
 * @return 0 on success, -ENOENT if nothing is in flight.
 */
int fake_ring_complete_next(fake_ring_t *fr, int res);

/**
 * @brief Complete a specific in-flight op with @p res.
 *
 * @param[in,out] fr  Fake ring.
 * @param[in]     op  The op to complete (must be in flight).
 * @param[in]     res Result to deliver.
 *
 * @return 0 on success, -ENOENT if @p op is not in flight.
 */
int fake_ring_complete(fake_ring_t *fr, op_t *op, int res);

/**
 * @brief Number of ops prepared but not yet submitted.
 *
 * @param[in] fr Fake ring.
 *
 * @return Pending count.
 */
size_t fake_ring_pending_count(const fake_ring_t *fr);

/**
 * @brief Number of submitted ops awaiting completion.
 *
 * @param[in] fr Fake ring.
 *
 * @return In-flight count.
 */
size_t fake_ring_inflight_count(const fake_ring_t *fr);

/**
 * @brief Return the type of the most recently prepared op.
 *
 * @param[in] fr Fake ring.
 *
 * @return The op type, or -1 if none was prepared.
 */
int fake_ring_last_type(const fake_ring_t *fr);

/**
 * @brief Submit all prepared ops (wraps the vtable submit()).
 *
 * @param[in,out] fr Fake ring.
 *
 * @return Number of ops moved to in-flight, or negative errno.
 */
int fake_ring_submit_all(fake_ring_t *fr);

/**
 * @brief Copy @p data into the op's recorded receive buffer, then complete it.
 *
 * Emulates a recv completion that delivered @p len bytes.
 *
 * @param[in,out] fr   Fake ring.
 * @param[in]     op   The in-flight recv op.
 * @param[in]     data Bytes to place in the op's buffer.
 * @param[in]     len  Number of bytes.
 *
 * @return 0 on success, -ENOENT if @p op is not in flight, -EINVAL if it has no
 *         buffer or @p len exceeds the recorded length.
 */
int fake_ring_fill_and_complete(fake_ring_t *fr, op_t *op, const void *data, size_t len);

/**
 * @brief Find the first in-flight op of a given type.
 *
 * @param[in] fr   Fake ring.
 * @param[in] type Operation type to match.
 *
 * @return The matching op, or NULL if none is in flight.
 */
op_t *fake_ring_find(const fake_ring_t *fr, op_type_t type);

#endif /* TG_TEST_FAKE_RING_H */
