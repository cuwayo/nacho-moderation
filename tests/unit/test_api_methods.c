/* test_api_methods.c - API method table invariants (SPEC-ADDENDUM-SSOT S4).
 *
 * Verifies the table derived from src/api_methods.def: names are unique and
 * non-empty, ids are stable, and the lookup and idempotency helpers agree with
 * the table.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <string.h>

#include <cmocka.h>

#include "api_methods.h"
#include "tables.h"

static void test_names_unique_and_nonempty(void **state)
{
    (void)state;
    for (size_t i = 0; i < (size_t)METHOD_COUNT; ++i) {
        assert_true(api_method_table[i].name[0] != '\0');
        assert_true(api_method_table[i].brief[0] != '\0');
        assert_int_equal(api_method_table[i].id, (method_id_t)i);
        for (size_t j = i + 1; j < (size_t)METHOD_COUNT; ++j) {
            assert_true(strcmp(api_method_table[i].name, api_method_table[j].name) != 0);
        }
    }
}

static void test_lookup(void **state)
{
    (void)state;
    const api_method_t *m = api_method_lookup("getMe");
    assert_non_null(m);
    assert_int_equal(m->id, METHOD_GET_ME);
    assert_int_equal(m->bucket, BUCKET_GLOBAL);
    assert_int_equal(m->idempotent, 1);

    assert_null(api_method_lookup("nope"));
    assert_null(api_method_lookup(NULL));
}

static void test_helpers(void **state)
{
    (void)state;
    assert_int_equal(api_method_is_idempotent(METHOD_GET_ME), 1);
    assert_int_equal(api_method_is_idempotent(METHOD_COUNT), 0);
    assert_string_equal(api_method_name(METHOD_GET_UPDATES), "getUpdates");
    assert_string_equal(api_method_name(METHOD_COUNT), "");
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_names_unique_and_nonempty),
        cmocka_unit_test(test_lookup),
        cmocka_unit_test(test_helpers),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
