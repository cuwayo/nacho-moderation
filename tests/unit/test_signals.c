/* test_signals.c - signalfd-via-io_uring signal delivery (A5.2).
 *
 * The watcher opens a real signalfd (so SIGINT/SIGTERM/SIGUSR1 are masked) but
 * the read is driven by the fake ring: the test raises a signal, fills the read
 * with a synthetic signalfd_siginfo and asserts the callback fires and the read
 * is re-armed.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <signal.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cmocka.h>

#include "fake_ring.h"
#include "tg/loop.h"
#include "tg/signals.h"

typedef struct {
    int calls;
    int last_signo;
} sink_t;

static void on_sig(int signo, void *ctx)
{
    sink_t *s = ctx;
    s->calls++;
    s->last_signo = signo;
}

static void test_signal_read_delivers_and_rearms(void **state)
{
    (void)state;
    fake_ring_t fr;
    const loop_ops_t *ops = fake_ring_init(&fr);
    sink_t sink = {0};

    signals_config_t cfg = {.ops = ops, .on_signal = on_sig, .ctx = &sink};
    signals_t *s = signals_create(&cfg);
    assert_non_null(s);
    assert_int_equal(signals_start(s), 0);
    assert_true(signals_fd(s) >= 0);

    /* The read is prepared; submit it and deliver a synthetic SIGUSR1. */
    assert_int_equal(ops->submit(ops->ctx), 1);
    op_t *read_op = fake_ring_find(&fr, OP_RECV);
    assert_non_null(read_op);
    struct signalfd_siginfo info;
    memset(&info, 0, sizeof(info));
    info.ssi_signo = SIGUSR1;
    assert_int_equal(fake_ring_fill_and_complete(&fr, read_op, &info, sizeof(info)), 0);
    assert_int_equal(sink.calls, 1);
    assert_int_equal(sink.last_signo, SIGUSR1);

    /* The read was re-armed for the next signal. */
    assert_int_equal(ops->submit(ops->ctx), 1);
    assert_non_null(fake_ring_find(&fr, OP_RECV));

    signals_destroy(s);
}

static void test_create_rejects_bad_args(void **state)
{
    (void)state;
    assert_null(signals_create(NULL));
    signals_config_t cfg = {0};
    assert_null(signals_create(&cfg)); /* no ops / no callback */
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_signal_read_delivers_and_rearms),
        cmocka_unit_test(test_create_rejects_bad_args),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
