/* test_cmd_help.c - the /help handler placeholder (S8 step 2).
 *
 * The router lands in M3, so cmd_help returns -ENOSYS for now; this test pins
 * that contract and keeps the handler compiled and linked.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <errno.h>

#include <cmocka.h>

#include "commands.h"

static void test_help_is_not_implemented_yet(void **state)
{
    (void)state;
    assert_int_equal(cmd_help(NULL), -ENOSYS);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_help_is_not_implemented_yet),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
