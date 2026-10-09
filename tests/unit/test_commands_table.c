/* test_commands_table.c - invariants of the command registry (SPEC-ADDENDUM-SSOT S2).
 *
 * Covers the S2 startup guarantees that are checked at build/test time:
 * names unique and matching ^[a-z0-9_]{1,32}$, brief length 1..256, handlers
 * non-NULL, ROLE_ANYONE implies RIGHT_NONE, and lookup by name.
 */
/* cmocka.h requires these to be included first. */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <string.h>

#include <cmocka.h>

#include "commands.h"
#include "tables.h"

/* Return 1 if s matches ^[a-z0-9_]{1,32}$ (no regex dependency needed). */
static int name_is_valid(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > 32) {
        return 0;
    }
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return 0;
        }
    }
    return 1;
}

static void test_count_is_positive(void **state)
{
    (void)state;
    assert_true(CMD_COUNT >= 1);
}

static void test_names_valid_and_unique(void **state)
{
    (void)state;
    for (size_t i = 0; i < (size_t)CMD_COUNT; ++i) {
        assert_true(name_is_valid(cmd_table[i].name));
        for (size_t j = i + 1; j < (size_t)CMD_COUNT; ++j) {
            assert_true(strcmp(cmd_table[i].name, cmd_table[j].name) != 0);
        }
    }
}

static void test_brief_length(void **state)
{
    (void)state;
    for (size_t i = 0; i < (size_t)CMD_COUNT; ++i) {
        size_t n = strlen(cmd_table[i].brief);
        assert_true(n >= 1);
        assert_true(n <= 256);
    }
}

static void test_handlers_non_null(void **state)
{
    (void)state;
    for (size_t i = 0; i < (size_t)CMD_COUNT; ++i) {
        assert_non_null(cmd_table[i].handler);
    }
}

static void test_anyone_role_has_no_right(void **state)
{
    (void)state;
    for (size_t i = 0; i < (size_t)CMD_COUNT; ++i) {
        if (cmd_table[i].min_role == ROLE_ANYONE) {
            assert_int_equal(cmd_table[i].user_right, RIGHT_NONE);
        }
    }
}

static void test_reply_required_and_target_not_both(void **state)
{
    (void)state;
    for (size_t i = 0; i < (size_t)CMD_COUNT; ++i) {
        unsigned f = cmd_table[i].flags;
        assert_false((f & CMDF_REPLY_REQUIRED) != 0u && (f & CMDF_TARGET) != 0u);
    }
}

static void test_lookup_finds_and_misses(void **state)
{
    (void)state;
    const cmd_desc_t *help = cmd_lookup("help");
    assert_non_null(help);
    assert_int_equal(help->id, CMD_HELP);
    assert_null(cmd_lookup("definitely_not_a_command"));
    assert_null(cmd_lookup(NULL));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_count_is_positive),
        cmocka_unit_test(test_names_valid_and_unique),
        cmocka_unit_test(test_brief_length),
        cmocka_unit_test(test_handlers_non_null),
        cmocka_unit_test(test_anyone_role_has_no_right),
        cmocka_unit_test(test_reply_required_and_target_not_both),
        cmocka_unit_test(test_lookup_finds_and_misses),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
