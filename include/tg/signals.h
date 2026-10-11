/**
 * @file signals.h
 * @brief Signal delivery through a signalfd read on the io_uring loop.
 *
 * SPEC section 8 requires signal handling via `signalfd` driven by io_uring, not
 * an asynchronous handler that pokes a flag. The blocking signals are masked
 * process-wide and read from a `signalfd(2)`; the read is submitted as a ring
 * read operation on the loop, so a SIGINT/SIGTERM wakes `io_uring_wait_cqe`
 * (which otherwise restarts itself on EINTR) and the loop dispatches the
 * notification on the loop thread.
 *
 * @ingroup loop
 */
#ifndef TG_SIGNALS_H
#define TG_SIGNALS_H

#include "tg/loop.h"

/** @brief Called when a watched signal arrives. */
typedef void (*signal_cb)(int signo, void *ctx);

/** @brief Configuration for the signal watcher. All pointers must outlive it. */
typedef struct {
    const loop_ops_t *ops; /**< Loop vtable (must expose @c prep_read). */
    signal_cb on_signal;   /**< Callback invoked on the loop thread (never NULL). */
    void *ctx;             /**< Opaque pointer passed to @p on_signal. */
} signals_config_t;

/** @brief The signal watcher (opaque; see signals.c). */
typedef struct signals signals_t;

/**
 * @brief Create the watcher: block SIGINT/SIGTERM/SIGUSR1 and open a signalfd.
 *
 * The signals are blocked with @c sigprocmask so they are delivered only through
 * the signalfd. Must be called before @ref signals_start.
 *
 * @param[in] cfg Configuration (copied; pointers must outlive the watcher).
 *
 * @return A new watcher, or NULL on failure.
 * @ownership Caller must call signals_destroy().
 * @thread_safety Call once, at startup, before the loop runs.
 */
signals_t *signals_create(const signals_config_t *cfg);

/**
 * @brief Arm the signalfd read.
 *
 * @param[in] s Watcher.
 *
 * @return 0 if armed, negative errno on failure.
 * @thread_safety Loop thread only.
 */
int signals_start(signals_t *s);

/**
 * @brief Return the signalfd, or -1 if not created.
 *
 * @param[in] s Watcher.
 *
 * @return The signalfd or -1.
 * @thread_safety Loop thread only.
 */
int signals_fd(const signals_t *s);

/**
 * @brief Close the signalfd and release the watcher.
 *
 * @param[in] s Watcher (may be NULL).
 * @thread_safety Loop thread only.
 */
void signals_destroy(signals_t *s);

#endif /* TG_SIGNALS_H */
