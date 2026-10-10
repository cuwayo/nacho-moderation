/* test_rate_limit.c - token-bucket rate limiter (SPEC-ADDENDUM-TOOLCHAIN A5.1).
 *
 * Deterministic: a fake clock drives refill and 429 pauses. Covers the global,
 * per-chat message and bulk limits, refill math, pause scoping, the ready-at
 * hint and the backoff schedule.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <errno.h>

#include <cmocka.h>

#include "tables.h"
#include "tg/rate_limit.h"
#include "tg/util.h"

/* --- Fake clock ------------------------------------------------------------ */

typedef struct {
    uint64_t now;
} fake_clock_t;

static uint64_t fc_now(void *ctx)
{
    return ((fake_clock_t *)ctx)->now;
}

static clock_ops_t make_clock(fake_clock_t *c)
{
    clock_ops_t ops = {.now_ms = fc_now, .ctx = c};
    return ops;
}

/* --- Tests ----------------------------------------------------------------- */

static void test_global_limit_25_per_second(void **state)
{
    (void)state;
    fake_clock_t c = {0};
    clock_ops_t clk = make_clock(&c);
    rate_limiter_t *lr = rate_limiter_create(&clk);
    assert_non_null(lr);

    for (int i = 0; i < 25; ++i) {
        assert_int_equal(rate_limiter_acquire(lr, BUCKET_GLOBAL, 0), 0);
    }
    assert_int_equal(rate_limiter_acquire(lr, BUCKET_GLOBAL, 0), -EAGAIN);

    /* 40 ms at 25 tokens/s refills exactly one token. */
    c.now += 40;
    assert_int_equal(rate_limiter_acquire(lr, BUCKET_GLOBAL, 0), 0);
    assert_int_equal(rate_limiter_acquire(lr, BUCKET_GLOBAL, 0), -EAGAIN);
    rate_limiter_destroy(lr);
}

static void test_per_chat_message_limit_15_per_minute(void **state)
{
    (void)state;
    fake_clock_t c = {0};
    clock_ops_t clk = make_clock(&c);
    rate_limiter_t *lr = rate_limiter_create(&clk);

    for (int i = 0; i < 15; ++i) {
        assert_int_equal(rate_limiter_acquire(lr, BUCKET_PER_CHAT, 100), 0);
    }
    assert_int_equal(rate_limiter_acquire(lr, BUCKET_PER_CHAT, 100), -EAGAIN);
    /* A different chat has its own bucket and is unaffected. */
    assert_int_equal(rate_limiter_acquire(lr, BUCKET_PER_CHAT, 200), 0);

    /* 4 s at 15/min refills exactly one token. */
    c.now += 4000;
    assert_int_equal(rate_limiter_acquire(lr, BUCKET_PER_CHAT, 100), 0);
    rate_limiter_destroy(lr);
}

static void test_bulk_limit_1_per_second(void **state)
{
    (void)state;
    fake_clock_t c = {0};
    clock_ops_t clk = make_clock(&c);
    rate_limiter_t *lr = rate_limiter_create(&clk);

    assert_int_equal(rate_limiter_acquire(lr, BUCKET_BULK, 5), 0);
    assert_int_equal(rate_limiter_acquire(lr, BUCKET_BULK, 5), -EAGAIN);
    c.now += 1000;
    assert_int_equal(rate_limiter_acquire(lr, BUCKET_BULK, 5), 0);
    rate_limiter_destroy(lr);
}

static void test_429_pause_global(void **state)
{
    (void)state;
    fake_clock_t c = {0};
    clock_ops_t clk = make_clock(&c);
    rate_limiter_t *lr = rate_limiter_create(&clk);

    rate_limiter_pause(lr, BUCKET_GLOBAL, 0, 500);
    assert_int_equal(rate_limiter_acquire(lr, BUCKET_GLOBAL, 0), -EAGAIN);
    c.now += 499;
    assert_int_equal(rate_limiter_acquire(lr, BUCKET_GLOBAL, 0), -EAGAIN);
    c.now += 1; /* 500 ms elapsed: pause lifted, tokens were full all along */
    assert_int_equal(rate_limiter_acquire(lr, BUCKET_GLOBAL, 0), 0);
    rate_limiter_destroy(lr);
}

static void test_429_pause_is_scoped_to_chat(void **state)
{
    (void)state;
    fake_clock_t c = {0};
    clock_ops_t clk = make_clock(&c);
    rate_limiter_t *lr = rate_limiter_create(&clk);

    rate_limiter_pause(lr, BUCKET_PER_CHAT, 100, 10000);
    assert_int_equal(rate_limiter_acquire(lr, BUCKET_PER_CHAT, 100), -EAGAIN);
    /* Other chats are unaffected by chat 100's pause. */
    assert_int_equal(rate_limiter_acquire(lr, BUCKET_PER_CHAT, 200), 0);
    rate_limiter_destroy(lr);
}

static void test_ready_at_hint(void **state)
{
    (void)state;
    fake_clock_t c = {0};
    clock_ops_t clk = make_clock(&c);
    rate_limiter_t *lr = rate_limiter_create(&clk);

    /* Full bucket: ready now. */
    assert_int_equal(rate_limiter_ready_at_ms(lr, BUCKET_GLOBAL, 0), 0);

    for (int i = 0; i < 25; ++i) {
        (void)rate_limiter_acquire(lr, BUCKET_GLOBAL, 0);
    }
    /* Empty: the next token is 40 ms away. */
    assert_int_equal(rate_limiter_ready_at_ms(lr, BUCKET_GLOBAL, 0), 40);

    /* A pause pushes the ready time out. */
    c.now += 100;
    rate_limiter_pause(lr, BUCKET_GLOBAL, 0, 500);
    assert_int_equal(rate_limiter_ready_at_ms(lr, BUCKET_GLOBAL, 0), c.now + 500);
    rate_limiter_destroy(lr);
}

static void test_backoff_schedule(void **state)
{
    (void)state;
    assert_int_equal(rate_limit_backoff_ms(0), 1000);
    assert_int_equal(rate_limit_backoff_ms(1), 2000);
    assert_int_equal(rate_limit_backoff_ms(2), 4000);
    assert_int_equal(rate_limit_backoff_ms(3), 8000);
    assert_int_equal(rate_limit_backoff_ms(4), 16000);
    assert_int_equal(rate_limit_backoff_ms(5), 30000);
    assert_int_equal(rate_limit_backoff_ms(99), 30000);
}

static void test_bad_arguments(void **state)
{
    (void)state;
    assert_null(rate_limiter_create(NULL));
    assert_int_equal(rate_limiter_acquire(NULL, BUCKET_GLOBAL, 0), -EINVAL);
    rate_limiter_destroy(NULL); /* must not crash */
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_global_limit_25_per_second),
        cmocka_unit_test(test_per_chat_message_limit_15_per_minute),
        cmocka_unit_test(test_bulk_limit_1_per_second),
        cmocka_unit_test(test_429_pause_global),
        cmocka_unit_test(test_429_pause_is_scoped_to_chat),
        cmocka_unit_test(test_ready_at_hint),
        cmocka_unit_test(test_backoff_schedule),
        cmocka_unit_test(test_bad_arguments),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
