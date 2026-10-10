/* test_cache.c - TTL cache behaviour (SPEC-ADDENDUM-TOOLCHAIN A5.1).
 *
 * Pure logic over an injected fake clock: put/get/overwrite/remove, TTL expiry,
 * capacity eviction (oldest first), and hash-collision handling.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "tg/cache.h"
#include "tg/util.h"

/* --- Fake clock ------------------------------------------------------------ */

static uint64_t fake_now_ms(void *ctx)
{
    return *(uint64_t *)ctx;
}

/* --- Tests ----------------------------------------------------------------- */

static void test_put_get_roundtrip(void **state)
{
    (void)state;
    uint64_t now = 1000;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    cache_t *c = cache_create(&clock, 8);
    assert_non_null(c);

    const char *v = "value-one";
    assert_int_equal(cache_put(c, "k1", v, strlen(v) + 1, 0), 0);

    size_t len = 0;
    const char *got = cache_get(c, "k1", &len);
    assert_non_null(got);
    assert_string_equal(got, v);
    assert_int_equal(len, strlen(v) + 1);
    assert_int_equal(cache_count(c), 1);

    cache_destroy(c);
}

static void test_missing_key_returns_null(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    cache_t *c = cache_create(&clock, 4);
    assert_null(cache_get(c, "nope", NULL));
    assert_null(cache_get(NULL, "nope", NULL));
    assert_int_equal(cache_remove(c, "nope"), 1);
    cache_destroy(c);
}

static void test_overwrite_replaces_value(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    cache_t *c = cache_create(&clock, 4);
    assert_int_equal(cache_put(c, "k", "a", 2, 0), 0);
    assert_int_equal(cache_put(c, "k", "bbb", 4, 0), 0);
    assert_int_equal(cache_count(c), 1);
    size_t len = 0;
    assert_string_equal((const char *)cache_get(c, "k", &len), "bbb");
    assert_int_equal(len, 4);
    cache_destroy(c);
}

static void test_ttl_expiry(void **state)
{
    (void)state;
    uint64_t now = 1000;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    cache_t *c = cache_create(&clock, 4);

    assert_int_equal(cache_put(c, "k", "v", 2, 500), 0); /* expires at 1500 */
    assert_non_null(cache_get(c, "k", NULL));

    /* Advance just before expiry: still present. */
    now = 1499;
    assert_int_equal(clock_now_ms(&clock), 1499);
    assert_non_null(cache_get(c, "k", NULL));

    /* Advance to exactly the expiry instant: the entry is gone. */
    // cppcheck-suppress redundantAssignment
    // (false positive: `now` is read through the injected clock callback.)
    now = 1500;
    assert_null(cache_get(c, "k", NULL));
    assert_int_equal(cache_count(c), 0);
    cache_destroy(c);
}

static void test_expire_sweeps(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    cache_t *c = cache_create(&clock, 8);
    assert_int_equal(cache_put(c, "short", "v", 2, 10), 0);
    assert_int_equal(cache_put(c, "forever", "v", 2, 0), 0);

    now = 20;
    cache_expire(c);
    assert_int_equal(cache_count(c), 1);
    assert_null(cache_get(c, "short", NULL));
    assert_non_null(cache_get(c, "forever", NULL));
    cache_destroy(c);
}

static void test_capacity_evicts_oldest(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    cache_t *c = cache_create(&clock, 2);
    assert_int_equal(cache_put(c, "a", "1", 2, 0), 0);
    assert_int_equal(cache_put(c, "b", "2", 2, 0), 0);
    assert_int_equal(cache_put(c, "c", "3", 2, 0), 0); /* evicts "a" */
    assert_int_equal(cache_count(c), 2);
    assert_null(cache_get(c, "a", NULL));
    assert_non_null(cache_get(c, "b", NULL));
    assert_non_null(cache_get(c, "c", NULL));
    cache_destroy(c);
}

static void test_remove(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    cache_t *c = cache_create(&clock, 4);
    assert_int_equal(cache_put(c, "k", "v", 2, 0), 0);
    assert_int_equal(cache_remove(c, "k"), 0);
    assert_int_equal(cache_count(c), 0);
    assert_null(cache_get(c, "k", NULL));
    cache_destroy(c);
}

static void test_collisions_distinct_keys(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    cache_t *c = cache_create(&clock, 64);
    /* Many keys force bucket collisions; every key must stay retrievable. */
    char key[16];
    char val[16];
    for (int i = 0; i < 40; ++i) {
        (void)snprintf(key, sizeof(key), "key%d", i);
        (void)snprintf(val, sizeof(val), "val%d", i);
        assert_int_equal(cache_put(c, key, val, strlen(val) + 1, 0), 0);
    }
    for (int i = 0; i < 40; ++i) {
        (void)snprintf(key, sizeof(key), "key%d", i);
        (void)snprintf(val, sizeof(val), "val%d", i);
        assert_string_equal((const char *)cache_get(c, key, NULL), val);
    }
    assert_int_equal(cache_count(c), 40);
    cache_destroy(c);
}

static void test_bad_args(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    cache_t *c = cache_create(&clock, 4);
    assert_int_equal(cache_put(NULL, "k", "v", 1, 0), -EINVAL);
    assert_int_equal(cache_put(c, NULL, "v", 1, 0), -EINVAL);
    assert_int_equal(cache_put(c, "k", NULL, 1, 0), -EINVAL);
    assert_null(cache_create(NULL, 4));
    cache_destroy(c);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_put_get_roundtrip),
        cmocka_unit_test(test_missing_key_returns_null),
        cmocka_unit_test(test_overwrite_replaces_value),
        cmocka_unit_test(test_ttl_expiry),
        cmocka_unit_test(test_expire_sweeps),
        cmocka_unit_test(test_capacity_evicts_oldest),
        cmocka_unit_test(test_remove),
        cmocka_unit_test(test_collisions_distinct_keys),
        cmocka_unit_test(test_bad_args),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
