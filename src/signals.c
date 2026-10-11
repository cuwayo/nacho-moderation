/**
 * @file signals.c
 * @brief signalfd-via-io_uring signal delivery (SPEC section 8).
 *
 * See signals.h. The three watched signals are blocked process-wide; the signalfd
 * read is a single io_uring read that is re-armed after every delivery. Because
 * the read is a real ring operation, an arriving signal produces a CQE and the
 * loop wakes even though `io_uring_wait_cqe` restarts itself on EINTR.
 *
 * @ingroup loop
 */
#include "tg/signals.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "tables.h"
#include "tg/loop.h"
#include "tg/util.h"

/** @brief The signals delivered through the signalfd. */
static const int SIGNALS_WATCHED[] = {SIGINT, SIGTERM, SIGUSR1};
/** @brief Number of entries in the watched-signal array. */
#define SIGNALS_WATCHED_COUNT (sizeof(SIGNALS_WATCHED) / sizeof(SIGNALS_WATCHED[0]))

/** @brief The signal watcher state. */
struct signals {
    signals_config_t cfg;         /**< Copied configuration. */
    int fd;                       /**< signalfd, or -1. */
    op_t read_op;                 /**< OP_RECV context for the signalfd read. */
    struct signalfd_siginfo info; /**< Destination for the read. */
    int armed;                    /**< 1 while a read is outstanding. */
    int stopped;                  /**< 1 once the watcher was torn down. */
};

/**
 * @brief Arm the signalfd read if not already outstanding.
 *
 * @param[in] s Watcher.
 */
static void arm_read(signals_t *s)
{
    if (s->stopped || s->armed) {
        return;
    }
    if (s->cfg.ops->prep_read(s->cfg.ops->ctx, &s->read_op, s->fd, &s->info, sizeof(s->info)) ==
        0) {
        s->armed = 1;
    }
}

/**
 * @brief signalfd read completion: report the signal and re-arm.
 *
 * @param[in] op  Read op (owner is the watcher).
 * @param[in] res Bytes read, or negative errno.
 */
static void on_signal_read(struct op *op, int res)
{
    signals_t *s = op->owner;
    s->armed = 0;
    if (res == (int)sizeof(s->info)) {
        int signo = (int)s->info.ssi_signo;
        if (s->cfg.on_signal != NULL) {
            s->cfg.on_signal(signo, s->cfg.ctx);
        }
    } else if (res < 0 && res != -EINTR && res != -EAGAIN) {
        util_log(LOG_WARN, "signal", "signalfd read failed: %d", res);
    }
    arm_read(s); /* keep one read outstanding so the next signal is not missed */
}

signals_t *signals_create(const signals_config_t *cfg)
{
    if (cfg == NULL || cfg->ops == NULL || cfg->ops->prep_read == NULL || cfg->on_signal == NULL) {
        return NULL;
    }
    signals_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->cfg = *cfg;
    s->fd = -1;

    /* Block the watched signals so they are only ever delivered via the fd.
     * pthread_sigmask is the correct call in a program that spawns threads: the
     * mask is inherited by the DNS and DB worker threads created later, so a
     * signal is never delivered to a worker instead of the signalfd. */
    sigset_t mask;
    sigemptyset(&mask);
    for (size_t i = 0; i < SIGNALS_WATCHED_COUNT; ++i) {
        sigaddset(&mask, SIGNALS_WATCHED[i]);
    }
    if (pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0) {
        free(s);
        return NULL;
    }

    /* Deliberately NOT SFD_NONBLOCK: the io_uring read must block in the kernel
     * until a signal arrives, otherwise it completes immediately with -EAGAIN and
     * busy-loops the event loop. */
    s->fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (s->fd < 0) {
        free(s);
        return NULL;
    }
    s->read_op = (op_t){.type = OP_RECV, .owner = s, .on_complete = on_signal_read, .fd = s->fd};
    return s;
}

int signals_start(signals_t *s)
{
    if (s == NULL) {
        return -EINVAL;
    }
    arm_read(s);
    return s->armed ? 0 : -EIO;
}

int signals_fd(const signals_t *s)
{
    return s == NULL ? -1 : s->fd;
}

void signals_destroy(signals_t *s)
{
    if (s == NULL) {
        return;
    }
    s->stopped = 1;
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    free(s);
}
