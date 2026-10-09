/* test_util.c - arena, strbuf, duration parser, HTML escape, clock (A5.1).
 *
 * Pure logic, no ring, no real clock for the parser/escape paths.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <errno.h>
#include <string.h>

#include <cmocka.h>

#include "tg/util.h"

/* --- Arena ----------------------------------------------------------------- */

static void test_arena_alloc_and_reset(void **state)
{
    (void)state;
    arena_t a;
    assert_int_equal(arena_init(&a, 128), 0);
    void *p1 = arena_alloc(&a, 32);
    void *p2 = arena_alloc(&a, 64);
    assert_non_null(p1);
    assert_non_null(p2);
    assert_true(p1 != p2);
    arena_reset(&a);
    void *p3 = arena_alloc(&a, 16);
    assert_non_null(p3);
    arena_destroy(&a);
}

static void test_arena_alignment(void **state)
{
    (void)state;
    arena_t a;
    assert_int_equal(arena_init(&a, 128), 0);
    for (int i = 1; i <= 8; ++i) {
        void *p = arena_alloc(&a, (size_t)i);
        assert_non_null(p);
        assert_int_equal((uintptr_t)p % 16u, 0);
    }
    arena_destroy(&a);
}

static void test_arena_growth_exhaustion(void **state)
{
    (void)state;
    arena_t a;
    assert_int_equal(arena_init(&a, 64), 0);
    /* Allocate well beyond one block; must keep working. */
    for (int i = 0; i < 100; ++i) {
        assert_non_null(arena_alloc(&a, 50));
    }
    arena_destroy(&a);
}

/* --- Strbuf ---------------------------------------------------------------- */

static void test_strbuf_append_and_format(void **state)
{
    (void)state;
    strbuf_t b;
    assert_int_equal(strbuf_init(&b, 0), 0);
    assert_int_equal(strbuf_append_str(&b, "abc"), 0);
    assert_int_equal(strbuf_append(&b, "def", 3), 0);
    assert_int_equal(strbuf_appendf(&b, "%d-%s", 42, "x"), 0);
    assert_string_equal(strbuf_cstr(&b), "abcdef42-x");
    strbuf_free(&b);
}

static void test_strbuf_grows(void **state)
{
    (void)state;
    strbuf_t b;
    assert_int_equal(strbuf_init(&b, 4), 0);
    for (int i = 0; i < 50; ++i) {
        assert_int_equal(strbuf_append_str(&b, "0123456789"), 0);
    }
    assert_int_equal(strlen(strbuf_cstr(&b)), 500);
    strbuf_free(&b);
}

/* --- Duration parser ------------------------------------------------------- */

static void test_duration_units(void **state)
{
    (void)state;
    struct {
        const char *in;
        int64_t want;
    } cases[] = {
        {"30s", 30}, {"10m", 600}, {"2h", 7200},  {"7d", 604800}, {"1w", 604800}, {"0", 0},
        {"45", 45},  {"1H", 3600}, {"1D", 86400}, {"1W", 604800}, {"1M", 60},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int64_t got = -1;
        assert_int_equal(util_parse_duration(cases[i].in, &got), 0);
        assert_int_equal(got, cases[i].want);
    }
}

static void test_duration_rejects_garbage(void **state)
{
    (void)state;
    const char *bad[] = {"", "abc", "10x", "1h30", "12s ", "s", "-5", "1.5h", " 10m"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        int64_t got = 0;
        assert_int_not_equal(util_parse_duration(bad[i], &got), 0);
    }
    assert_int_equal(util_parse_duration(NULL, &(int64_t){0}), -EINVAL);
}

static void test_duration_overflow(void **state)
{
    (void)state;
    int64_t got = 0;
    assert_int_equal(util_parse_duration("9223372036854775807w", &got), -ERANGE);
}

/* --- HTML escape ----------------------------------------------------------- */

static void test_html_escape_basic(void **state)
{
    (void)state;
    char out[64];
    size_t n = 0;
    assert_int_equal(util_html_escape("a&b<c>d\"e", 9, out, sizeof(out), &n), 0);
    assert_string_equal(out, "a&amp;b&lt;c&gt;d&quot;e");
    assert_int_equal(n, strlen(out));
}

static void test_html_escape_utf8_passthrough(void **state)
{
    (void)state;
    char out[32];
    size_t n = 0;
    const char *utf8 = "\xc3\xa9\xe2\x9c\x93"; /* é ✓ */
    assert_int_equal(util_html_escape(utf8, 5, out, sizeof(out), &n), 0);
    assert_int_equal(n, 5);
    assert_memory_equal(out, utf8, 5);
}

static void test_html_escape_truncation(void **state)
{
    (void)state;
    char out[8];
    size_t n = 0;
    /* "&&&" expands to "&amp;&amp;&amp;" (15 bytes) which cannot fit in 8. */
    int rc = util_html_escape("&&&", 3, out, sizeof(out), &n);
    assert_int_equal(rc, -ENOSPC);
    assert_true(n < sizeof(out));
    /* The result is NUL-terminated at n (checked via a bounded scan). */
    assert_int_equal(strnlen(out, sizeof(out)), n);
}

static void test_html_escape_boundary(void **state)
{
    (void)state;
    char out[6]; /* exactly fits "&amp;" + NUL */
    size_t n = 0;
    assert_int_equal(util_html_escape("&", 1, out, sizeof(out), &n), 0);
    assert_string_equal(out, "&amp;");
}

/* --- Clock ----------------------------------------------------------------- */

static uint64_t fake_clock(void *ctx)
{
    return *(uint64_t *)ctx;
}

static void test_clock_injection(void **state)
{
    (void)state;
    uint64_t t = 12345;
    clock_ops_t c = {.now_ms = fake_clock, .ctx = &t};
    assert_int_equal(clock_now_ms(&c), 12345);
    assert_int_equal(clock_now_ms(NULL), 0);
    assert_true(clock_now_ms(&util_clock_ops_real) > 0);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_arena_alloc_and_reset),
        cmocka_unit_test(test_arena_alignment),
        cmocka_unit_test(test_arena_growth_exhaustion),
        cmocka_unit_test(test_strbuf_append_and_format),
        cmocka_unit_test(test_strbuf_grows),
        cmocka_unit_test(test_duration_units),
        cmocka_unit_test(test_duration_rejects_garbage),
        cmocka_unit_test(test_duration_overflow),
        cmocka_unit_test(test_html_escape_basic),
        cmocka_unit_test(test_html_escape_utf8_passthrough),
        cmocka_unit_test(test_html_escape_truncation),
        cmocka_unit_test(test_html_escape_boundary),
        cmocka_unit_test(test_clock_injection),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
