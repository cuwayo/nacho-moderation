/* test_loop_dispatch.c - CQE dispatch by op type over the fake ring (A5.2).
 *
 * Verifies that a completion invokes the op's callback with the right result,
 * that batching submits once, and that -EINTR/-ECANCELED/-ECONNRESET propagate.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <errno.h>

#include <cmocka.h>

#include "fake_ring.h"
#include "tg/loop.h"

typedef struct {
    int calls;
    int last_res;
    op_type_t last_type;
} sink_t;

static void on_done(struct op *op, int res)
{
    sink_t *s = op->owner;
    s->calls++;
    s->last_res = res;
    s->last_type = op->type;
}

static void test_dispatch_invokes_callback(void **state)
{
    (void)state;
    fake_ring_t fr;
    const loop_ops_t *ops = fake_ring_init(&fr);
    sink_t sink = {0};
    op_t op = {.type = OP_RECV, .owner = &sink, .on_complete = on_done, .fd = 7};

    assert_int_equal(ops->prep_recv(ops->ctx, &op, 7, NULL, 0), 0);
    assert_int_equal(ops->submit(ops->ctx), 1);
    assert_int_equal(fake_ring_complete_next(&fr, 123), 0);
    assert_int_equal(sink.calls, 1);
    assert_int_equal(sink.last_res, 123);
    assert_int_equal(sink.last_type, OP_RECV);
}

static void test_batching_counts_submits(void **state)
{
    (void)state;
    fake_ring_t fr;
    const loop_ops_t *ops = fake_ring_init(&fr);
    sink_t sink = {0};
    op_t a = {.type = OP_SEND, .owner = &sink, .on_complete = on_done};
    op_t b = {.type = OP_RECV, .owner = &sink, .on_complete = on_done};
    op_t c = {.type = OP_TIMEOUT, .owner = &sink, .on_complete = on_done};

    assert_int_equal(ops->prep_send(ops->ctx, &a, 1, "x", 1), 0);
    assert_int_equal(ops->prep_recv(ops->ctx, &b, 1, NULL, 0), 0);
    assert_int_equal(ops->prep_timeout(ops->ctx, &c, 100), 0);
    assert_int_equal(fake_ring_pending_count(&fr), 3);
    assert_int_equal(ops->submit(ops->ctx), 3); /* one submit for all three */
    assert_int_equal(fake_ring_inflight_count(&fr), 3);
}

static void test_error_results_propagate(void **state)
{
    (void)state;
    const int errs[] = {-EINTR, -EAGAIN, -ECANCELED, -ECONNRESET};
    for (size_t i = 0; i < sizeof(errs) / sizeof(errs[0]); ++i) {
        fake_ring_t fr;
        const loop_ops_t *ops = fake_ring_init(&fr);
        sink_t sink = {0};
        op_t op = {.type = OP_RECV, .owner = &sink, .on_complete = on_done};
        assert_int_equal(ops->prep_recv(ops->ctx, &op, 1, NULL, 0), 0);
        assert_int_equal(ops->submit(ops->ctx), 1);
        assert_int_equal(fake_ring_complete_next(&fr, errs[i]), 0);
        assert_int_equal(sink.last_res, errs[i]);
    }
}

static void test_find_and_fill(void **state)
{
    (void)state;
    fake_ring_t fr;
    const loop_ops_t *ops = fake_ring_init(&fr);
    sink_t sink = {0};
    char buf[16] = {0};
    op_t op = {.type = OP_RECV, .owner = &sink, .on_complete = on_done};
    assert_int_equal(ops->prep_recv(ops->ctx, &op, 1, buf, sizeof(buf)), 0);
    assert_int_equal(ops->submit(ops->ctx), 1);
    assert_ptr_equal(fake_ring_find(&fr, OP_RECV), &op);
    assert_null(fake_ring_find(&fr, OP_SEND));
    assert_int_equal(fake_ring_fill_and_complete(&fr, &op, "hello", 5), 0);
    assert_memory_equal(buf, "hello", 5);
    assert_int_equal(sink.last_res, 5);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_dispatch_invokes_callback),
        cmocka_unit_test(test_batching_counts_submits),
        cmocka_unit_test(test_error_results_propagate),
        cmocka_unit_test(test_find_and_fill),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
