/* test_router.c - command parsing and target resolution (A5.1).
 *
 * Pure logic: exercises `/cmd@bot`, case folding, argument splitting, the
 * reply-target and numeric-id resolution order, and ignoring commands addressed
 * to another bot. No network, no DB.
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

#include "tg/router.h"
#include "tg/tg_types.h"

static void test_plain_text_is_not_a_command(void **state)
{
    (void)state;
    cmd_invocation_t inv;
    assert_int_equal(router_parse_command("hello world", "MyBot", &inv), 0);
    assert_int_equal(router_parse_command("", "MyBot", &inv), 0);
    assert_int_equal(router_parse_command("/", "MyBot", &inv), 0);
    assert_int_equal(router_parse_command(NULL, "MyBot", &inv), -EINVAL);
}

static void test_simple_command_and_args(void **state)
{
    (void)state;
    cmd_invocation_t inv;
    assert_int_equal(router_parse_command("/ban 30m spam", "MyBot", &inv), 1);
    assert_string_equal(inv.name, "ban");
    assert_false(inv.addressed_to_other_bot);
    assert_string_equal(inv.args, "30m spam");
}

static void test_case_folding(void **state)
{
    (void)state;
    cmd_invocation_t inv;
    assert_int_equal(router_parse_command("/BAN", "MyBot", &inv), 1);
    assert_string_equal(inv.name, "ban");
}

static void test_addressed_to_own_bot(void **state)
{
    (void)state;
    cmd_invocation_t inv;
    assert_int_equal(router_parse_command("/help@MyBot", "MyBot", &inv), 1);
    assert_string_equal(inv.name, "help");
    assert_false(inv.addressed_to_other_bot);
    assert_string_equal(inv.args, "");
}

static void test_addressed_to_other_bot(void **state)
{
    (void)state;
    cmd_invocation_t inv;
    assert_int_equal(router_parse_command("/help@OtherBot", "MyBot", &inv), 1);
    assert_string_equal(inv.name, "help");
    assert_true(inv.addressed_to_other_bot);
}

static void test_username_target(void **state)
{
    (void)state;
    cmd_invocation_t inv;
    assert_int_equal(router_parse_command("/ban @spammer 10m reason", "MyBot", &inv), 1);
    assert_string_equal(inv.name, "ban");
    assert_true(inv.has_target);
    assert_string_equal(inv.target, "@spammer");
    assert_string_equal(inv.args, "10m reason");
}

static void test_numeric_target(void **state)
{
    (void)state;
    cmd_invocation_t inv;
    assert_int_equal(router_parse_command("/ban 123456 10m", "MyBot", &inv), 1);
    assert_true(inv.has_target);
    assert_string_equal(inv.target, "123456");
    assert_string_equal(inv.args, "10m");
}

static void test_resolve_reply_wins_over_target(void **state)
{
    (void)state;
    cmd_invocation_t inv;
    (void)router_parse_command("/ban 999", "MyBot", &inv);
    tg_message_t msg = {.has_reply = 1, .reply_from = {.id = 555}};
    int64_t out = 0;
    assert_int_equal(router_resolve_target(&inv, &msg, &out), 0);
    assert_int_equal(out, 555); /* reply target beats the numeric argument */
}

static void test_resolve_numeric_when_no_reply(void **state)
{
    (void)state;
    cmd_invocation_t inv;
    (void)router_parse_command("/ban 123456", "MyBot", &inv);
    tg_message_t msg = {0};
    int64_t out = 0;
    assert_int_equal(router_resolve_target(&inv, &msg, &out), 0);
    assert_int_equal(out, 123456);
}

static void test_resolve_no_target(void **state)
{
    (void)state;
    cmd_invocation_t inv;
    (void)router_parse_command("/help", "MyBot", &inv);
    tg_message_t msg = {0};
    int64_t out = -1;
    assert_int_equal(router_resolve_target(&inv, &msg, &out), 1);
    assert_int_equal(out, 0);
}

static void test_resolve_username_unsupported(void **state)
{
    (void)state;
    cmd_invocation_t inv;
    (void)router_parse_command("/ban @who", "MyBot", &inv);
    tg_message_t msg = {0};
    int64_t out = 0;
    assert_int_equal(router_resolve_target(&inv, &msg, &out), -ENOTSUP);
}

static void test_resolve_bad_args(void **state)
{
    (void)state;
    cmd_invocation_t inv = {0};
    tg_message_t msg = {0};
    int64_t out = 0;
    assert_int_equal(router_resolve_target(NULL, &msg, &out), -EINVAL);
    assert_int_equal(router_resolve_target(&inv, NULL, &out), -EINVAL);
    assert_int_equal(router_resolve_target(&inv, &msg, NULL), -EINVAL);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_plain_text_is_not_a_command),
        cmocka_unit_test(test_simple_command_and_args),
        cmocka_unit_test(test_case_folding),
        cmocka_unit_test(test_addressed_to_own_bot),
        cmocka_unit_test(test_addressed_to_other_bot),
        cmocka_unit_test(test_username_target),
        cmocka_unit_test(test_numeric_target),
        cmocka_unit_test(test_resolve_reply_wins_over_target),
        cmocka_unit_test(test_resolve_numeric_when_no_reply),
        cmocka_unit_test(test_resolve_no_target),
        cmocka_unit_test(test_resolve_username_unsupported),
        cmocka_unit_test(test_resolve_bad_args),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
