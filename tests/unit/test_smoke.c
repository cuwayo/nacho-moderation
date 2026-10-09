/* test_smoke.c - the M0 trivial passing test (SPEC-ADDENDUM-TOOLCHAIN A10).
 *
 * Confirms the cmocka + ctest + sanitizer wiring works end-to-end. Real module
 * tests replace/extend this as each M1+ module lands.
 */
/* cmocka.h requires these to be included first (it uses jmp_buf, va_list,
 * size_t and uintptr_t); include-cleaner cannot see that through its macros. */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <cmocka.h>

#include "tg/version.h"

static void test_version_is_nonempty(void **state)
{
    (void)state;
    const char *v = tg_version();
    assert_non_null(v);
    assert_true(v[0] != '\0');
}

static void test_version_matches_macro(void **state)
{
    (void)state;
    assert_string_equal(tg_version(), TG_VERSION_STRING);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_version_is_nonempty),
        cmocka_unit_test(test_version_matches_macro),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
