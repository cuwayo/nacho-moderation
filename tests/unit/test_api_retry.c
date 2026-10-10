/* test_api_retry.c - retry policy decisions (SPEC-ADDENDUM-TOOLCHAIN A5.1).
 *
 * Pure logic: idempotent vs non-idempotent 5xx/network handling, 429 requeue,
 * the retry cap, and the deadline stop.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <cmocka.h>

#include "api_methods.h"
#include "tg/api_retry.h"
#include "tg/errors.h"

static void test_idempotent_5xx_retries_with_backoff(void **state)
{
    (void)state;
    assert_int_equal(api_retry_decide(METHOD_GET_ME, -ERR_HTTP, 0, 0, 0), API_RETRY_BACKOFF);
    assert_int_equal(api_retry_decide(METHOD_GET_ME, -ERR_NET, 0, 0, 0), API_RETRY_BACKOFF);
}

static void test_retry_cap(void **state)
{
    (void)state;
    assert_int_equal(api_retry_decide(METHOD_GET_UPDATES, -ERR_NET, API_MAX_RETRIES - 1, 0, 0),
                     API_RETRY_BACKOFF);
    assert_int_equal(api_retry_decide(METHOD_GET_UPDATES, -ERR_NET, API_MAX_RETRIES, 0, 0),
                     API_RETRY_NO);
}

static void test_429_requeues_now(void **state)
{
    (void)state;
    assert_int_equal(api_retry_decide(METHOD_GET_ME, -ERR_RATE_LIMIT, 0, 0, 0), API_RETRY_NOW);
    /* Even at the retry cap, 429 is honoured (the pause bounds it, not the count). */
    assert_int_equal(api_retry_decide(METHOD_GET_ME, -ERR_RATE_LIMIT, 99, 0, 0), API_RETRY_NOW);
}

static void test_4xx_and_parse_never_retry(void **state)
{
    (void)state;
    assert_int_equal(api_retry_decide(METHOD_GET_ME, -ERR_TG_API, 0, 0, 0), API_RETRY_NO);
    assert_int_equal(api_retry_decide(METHOD_GET_ME, -ERR_PARSE, 0, 0, 0), API_RETRY_NO);
    assert_int_equal(api_retry_decide(METHOD_GET_ME, -ERR_TLS, 0, 0, 0), API_RETRY_NO);
}

static void test_deadline_stops_retries(void **state)
{
    (void)state;
    assert_int_equal(api_retry_decide(METHOD_GET_ME, -ERR_NET, 0, 1000, 500), API_RETRY_NO);
    assert_int_equal(api_retry_decide(METHOD_GET_ME, -ERR_NET, 0, 500, 1000), API_RETRY_BACKOFF);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_idempotent_5xx_retries_with_backoff),
        cmocka_unit_test(test_retry_cap),
        cmocka_unit_test(test_429_requeues_now),
        cmocka_unit_test(test_4xx_and_parse_never_retry),
        cmocka_unit_test(test_deadline_stops_retries),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
