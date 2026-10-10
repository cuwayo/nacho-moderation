/* test_cmd_util.c - moderation argument parsing and target guards (A5.1, A5.4).
 *
 * Pure logic: `[duration] [reason]` splitting and the target guard that rejects
 * the owner, other admins and the bot itself. No network, no DB.
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

#include "commands.h"
#include "tables.h"
#include "tg/cmd_ctx.h"
#include "tg/cmd_util.h"
#include "tg/errors.h"

static void test_duration_and_reason(void **state)
{
    (void)state;
    int64_t d = -1;
    const char *reason = NULL;
    assert_int_equal(cmd_parse_duration_reason("10m spamming", &d, &reason), 0);
    assert_int_equal(d, 600);
    assert_string_equal(reason, "spamming");

    assert_int_equal(cmd_parse_duration_reason("2h", &d, &reason), 0);
    assert_int_equal(d, 7200);
    assert_string_equal(reason, "");
}

static void test_reason_only(void **state)
{
    (void)state;
    int64_t d = -1;
    const char *reason = NULL;
    /* "spam 10m" is a reason, not a duration: the token must be first. */
    assert_int_equal(cmd_parse_duration_reason("spam 10m", &d, &reason), 0);
    assert_int_equal(d, 0);
    assert_string_equal(reason, "spam 10m");

    assert_int_equal(cmd_parse_duration_reason("", &d, &reason), 0);
    assert_int_equal(d, 0);
    assert_string_equal(reason, "");

    assert_int_equal(cmd_parse_duration_reason("   ", &d, &reason), 0);
    assert_int_equal(d, 0);
    assert_string_equal(reason, "");
}

static void test_duration_forms(void **state)
{
    (void)state;
    int64_t d = 0;
    const char *reason = NULL;
    assert_int_equal(cmd_parse_duration_reason("30s x", &d, &reason), 0);
    assert_int_equal(d, 30);
    assert_string_equal(reason, "x");
    assert_int_equal(cmd_parse_duration_reason("7d", &d, &reason), 0);
    assert_int_equal(d, 604800);
    assert_int_equal(cmd_parse_duration_reason("1w", &d, &reason), 0);
    assert_int_equal(d, 604800);
    /* A bare integer is seconds. */
    assert_int_equal(cmd_parse_duration_reason("90 reason", &d, &reason), 0);
    assert_int_equal(d, 90);
    assert_string_equal(reason, "reason");
    /* Zero parses as permanent (0 seconds). */
    assert_int_equal(cmd_parse_duration_reason("0", &d, &reason), 0);
    assert_int_equal(d, 0);
}

static void test_parse_bad_args(void **state)
{
    (void)state;
    int64_t d = 0;
    const char *reason = NULL;
    assert_int_equal(cmd_parse_duration_reason(NULL, &d, &reason), -EINVAL);
    assert_int_equal(cmd_parse_duration_reason("x", NULL, &reason), -EINVAL);
    assert_int_equal(cmd_parse_duration_reason("x", &d, NULL), -EINVAL);
}

static void test_target_guard(void **state)
{
    (void)state;
    struct cmd_ctx ctx = {.bot_id = 999, .target_id = 5, .target_role = ROLE_ANYONE};
    assert_int_equal(cmd_target_guard(&ctx), 0); /* plain member ok */

    ctx.target_id = 999; /* the bot itself */
    assert_int_equal(cmd_target_guard(&ctx), -ERR_PERM);

    ctx.target_id = 5;
    ctx.target_role = ROLE_ADMIN;
    assert_int_equal(cmd_target_guard(&ctx), -ERR_PERM);

    ctx.target_role = ROLE_OWNER;
    assert_int_equal(cmd_target_guard(&ctx), -ERR_PERM);
}

static void test_target_guard_bad_args(void **state)
{
    (void)state;
    assert_int_equal(cmd_target_guard(NULL), -EINVAL);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_duration_and_reason), cmocka_unit_test(test_reason_only),
        cmocka_unit_test(test_duration_forms),      cmocka_unit_test(test_parse_bad_args),
        cmocka_unit_test(test_target_guard),        cmocka_unit_test(test_target_guard_bad_args),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
