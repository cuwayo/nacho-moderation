/* test_tg_api.c - getMe JSON parsing and client URL handling (A5.1).
 *
 * Pure logic: parsing needs no network. The end-to-end client is covered by the
 * integration test against the mock server.
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

#include "tg/errors.h"
#include "tg/tg_api.h"

static void test_parse_me_ok(void **state)
{
    (void)state;
    const char *json = "{\"ok\":true,\"result\":{\"id\":123,\"is_bot\":true,"
                       "\"first_name\":\"Nacho\",\"username\":\"nacho_bot\"}}";
    tg_me_t me;
    assert_int_equal(tg_api_parse_me(json, strlen(json), &me), 0);
    assert_int_equal(me.id, 123);
    assert_true(me.is_bot);
    assert_string_equal(me.first_name, "Nacho");
    assert_string_equal(me.username, "nacho_bot");
}

static void test_parse_me_large_negative_id(void **state)
{
    (void)state;
    /* Bots have positive ids, but the parser must handle int64 cleanly. */
    const char *json =
        "{\"ok\":true,\"result\":{\"id\":-1001234567890,\"is_bot\":true,\"first_name\":\"X\"}}";
    tg_me_t me;
    assert_int_equal(tg_api_parse_me(json, strlen(json), &me), 0);
    assert_int_equal(me.id, -1001234567890LL);
}

static void test_parse_me_missing_fields(void **state)
{
    (void)state;
    const char *json = "{\"ok\":true,\"result\":{\"id\":7}}";
    tg_me_t me;
    assert_int_equal(tg_api_parse_me(json, strlen(json), &me), 0);
    assert_int_equal(me.id, 7);
    assert_false(me.is_bot);
    assert_string_equal(me.first_name, "");
}

static void test_parse_me_not_ok(void **state)
{
    (void)state;
    const char *json = "{\"ok\":false,\"description\":\"Unauthorized\"}";
    tg_me_t me;
    assert_int_equal(tg_api_parse_me(json, strlen(json), &me), -ERR_PARSE);
}

static void test_parse_me_garbage(void **state)
{
    (void)state;
    tg_me_t me;
    assert_int_equal(tg_api_parse_me("not json", 8, &me), -ERR_PARSE);
    assert_int_equal(tg_api_parse_me("{}", 2, &me), -ERR_PARSE);
    assert_int_equal(tg_api_parse_me(NULL, 0, &me), -EINVAL);
}

static void test_parse_me_extra_fields_ignored(void **state)
{
    (void)state;
    const char *json = "{\"ok\":true,\"extra\":1,\"result\":{\"id\":1,\"is_bot\":true,"
                       "\"first_name\":\"A\",\"username\":\"a\",\"can_join_groups\":true}}";
    tg_me_t me;
    assert_int_equal(tg_api_parse_me(json, strlen(json), &me), 0);
    assert_string_equal(me.username, "a");
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_parse_me_ok),
        cmocka_unit_test(test_parse_me_large_negative_id),
        cmocka_unit_test(test_parse_me_missing_fields),
        cmocka_unit_test(test_parse_me_not_ok),
        cmocka_unit_test(test_parse_me_garbage),
        cmocka_unit_test(test_parse_me_extra_fields_ignored),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
