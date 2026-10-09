/* test_cli.c - argument parsing and help output (SPEC-ADDENDUM-SSOT S5).
 *
 * Covers every flag, the value options, error paths, and the golden --help text.
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

#include "cli.h"

static int parse(char **argv, cli_options_t *opts)
{
    int argc = 0;
    while (argv[argc] != NULL) {
        ++argc;
    }
    return cli_parse(argc, argv, opts);
}

static void test_help_and_version(void **state)
{
    (void)state;
    cli_options_t o;
    char *h[] = {"tgbot", "--help", NULL};
    assert_int_equal(parse(h, &o), 0);
    assert_int_equal(o.action, CLI_ACTION_HELP);
    char *sh[] = {"tgbot", "-h", NULL};
    assert_int_equal(parse(sh, &o), 0);
    assert_int_equal(o.action, CLI_ACTION_HELP);
    char *v[] = {"tgbot", "--version", NULL};
    assert_int_equal(parse(v, &o), 0);
    assert_int_equal(o.action, CLI_ACTION_VERSION);
    char *sv[] = {"tgbot", "-V", NULL};
    assert_int_equal(parse(sv, &o), 0);
    assert_int_equal(o.action, CLI_ACTION_VERSION);
}

static void test_dump_flags(void **state)
{
    (void)state;
    struct {
        const char *flag;
        cli_action_t action;
    } cases[] = {
        {"--dump-commands-json", CLI_ACTION_DUMP_COMMANDS_JSON},
        {"--dump-config-json", CLI_ACTION_DUMP_CONFIG_JSON},
        {"--dump-tables-json", CLI_ACTION_DUMP_TABLES_JSON},
        {"--dump-config-example", CLI_ACTION_DUMP_CONFIG_EXAMPLE},
        {"--self-test", CLI_ACTION_SELF_TEST},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        cli_options_t o;
        char *argv[] = {"tgbot", (char *)cases[i].flag, NULL};
        assert_int_equal(parse(argv, &o), 0);
        assert_int_equal(o.action, cases[i].action);
    }
}

static void test_value_options(void **state)
{
    (void)state;
    cli_options_t o;
    char *c[] = {"tgbot", "--config", "/etc/bot.toml", NULL};
    assert_int_equal(parse(c, &o), 0);
    assert_int_equal(o.action, CLI_ACTION_RUN);
    assert_string_equal(o.config_path, "/etc/bot.toml");

    char *l[] = {"tgbot", "--log-level", "debug", NULL};
    assert_int_equal(parse(l, &o), 0);
    assert_string_equal(o.log_level, "debug");

    char *k[] = {"tgbot", "--check-config", "x.toml", NULL};
    assert_int_equal(parse(k, &o), 0);
    assert_int_equal(o.action, CLI_ACTION_CHECK_CONFIG);
    assert_string_equal(o.check_path, "x.toml");
}

static void test_errors(void **state)
{
    (void)state;
    cli_options_t o;
    char *missing[] = {"tgbot", "--config", NULL};
    assert_int_equal(parse(missing, &o), -EINVAL);
    char *unknown[] = {"tgbot", "--nope", NULL};
    assert_int_equal(parse(unknown, &o), -EINVAL);
    assert_int_equal(cli_parse(0, NULL, &o), -EINVAL);
}

static void test_help_text(void **state)
{
    (void)state;
    FILE *f = tmpfile();
    if (f == NULL) {
        fail_msg("tmpfile failed");
        return;
    }
    cli_print_help(f);
    if (fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        fail_msg("fseek failed");
        return;
    }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    (void)fclose(f);
    assert_non_null(strstr(buf, "Usage:"));
    assert_non_null(strstr(buf, "--config <file>"));
    assert_non_null(strstr(buf, "--check-config <file>"));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_help_and_version), cmocka_unit_test(test_dump_flags),
        cmocka_unit_test(test_value_options),    cmocka_unit_test(test_errors),
        cmocka_unit_test(test_help_text),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
