/* test_config_table.c - configuration schema and validation (SPEC-ADDENDUM-SSOT S3).
 *
 * Covers schema invariants (unique keys/env vars, enum keys have values, required
 * keys exist) and the --check-config round-trip: the generated sample validates,
 * an unknown key fails, a bad enum value fails, and a missing required key fails.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

#include "config.h"
#include "tables.h"

static void test_keys_and_env_unique(void **state)
{
    (void)state;
    for (size_t i = 0; i < (size_t)CFG_COUNT; ++i) {
        assert_true(config_table[i].key[0] != '\0');
        assert_true(config_table[i].env[0] != '\0');
        for (size_t j = i + 1; j < (size_t)CFG_COUNT; ++j) {
            assert_true(strcmp(config_table[i].key, config_table[j].key) != 0);
            assert_true(strcmp(config_table[i].env, config_table[j].env) != 0);
        }
    }
}

static void test_enum_keys_have_values(void **state)
{
    (void)state;
    for (size_t i = 0; i < (size_t)CFG_COUNT; ++i) {
        if (config_table[i].type != CFG_ENUM) {
            continue;
        }
        const cfg_enum_row_t *rows = NULL;
        size_t count = 0;
        assert_int_equal(config_enum_values(config_table[i].id, &rows, &count), 0);
        assert_non_null(rows);
        assert_true(count >= 1);
    }
}

static void test_required_bot_token(void **state)
{
    (void)state;
    assert_int_equal(config_is_required(CFG_BOT_TOKEN), 1);
    assert_int_equal(config_is_required(CFG_MODE), 0);
}

static void test_lookup_by_key_and_env(void **state)
{
    (void)state;
    const cfg_desc_t *by_key = config_lookup_key("api.pool_size");
    assert_non_null(by_key);
    assert_int_equal(by_key->id, CFG_API_POOL_SIZE);

    const cfg_desc_t *by_env = config_lookup_env("BOT_TOKEN");
    assert_non_null(by_env);
    assert_int_equal(by_env->id, CFG_BOT_TOKEN);

    assert_null(config_lookup_key("no.such.key"));
    assert_null(config_lookup_env("NOPE"));
}

/* Write text to a fresh temp file; caller frees *path. */
static int write_temp(const char *text, char **path)
{
    char tmpl[] = "/tmp/nm_cfg_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    size_t len = strlen(text);
    if (write(fd, text, len) != (ssize_t)len) {
        close(fd);
        return -1;
    }
    close(fd);
    *path = strdup(tmpl);
    return 0;
}

static void test_sample_config_is_valid(void **state)
{
    (void)state;
    char *path = NULL;
    assert_int_equal(write_temp("bot_token = \"123456:ABC\"\n"
                                "mode = \"polling\"\n"
                                "[api]\npool_size = 4\n",
                                &path),
                     0);
    char err[256] = "";
    assert_int_equal(config_check_file(path, err, sizeof(err)), 0);
    free(path);
}

static void test_unknown_key_rejected(void **state)
{
    (void)state;
    char *path = NULL;
    assert_int_equal(write_temp("bot_token = \"x\"\nbogus_key = 1\n", &path), 0);
    char err[256] = "";
    assert_int_not_equal(config_check_file(path, err, sizeof(err)), 0);
    free(path);
}

static void test_bad_enum_rejected(void **state)
{
    (void)state;
    char *path = NULL;
    assert_int_equal(write_temp("bot_token = \"x\"\nmode = \"carrier-pigeon\"\n", &path), 0);
    char err[256] = "";
    assert_int_not_equal(config_check_file(path, err, sizeof(err)), 0);
    assert_non_null(strstr(err, "invalid value"));
    free(path);
}

static void test_missing_required_rejected(void **state)
{
    (void)state;
    char *path = NULL;
    assert_int_equal(write_temp("mode = \"polling\"\n", &path), 0);
    char err[256] = "";
    assert_int_not_equal(config_check_file(path, err, sizeof(err)), 0);
    assert_non_null(strstr(err, "missing required"));
    free(path);
}

static void test_load_defaults_and_getters(void **state)
{
    (void)state;
    config_values_t v;
    assert_int_equal(config_load(NULL, NULL, &v), 0);
    /* Defaults are applied for keys with a default. */
    assert_string_equal(config_get_str(&v, CFG_MODE), "polling");
    assert_string_equal(config_get_str(&v, CFG_LOG_LEVEL), "info");
    assert_int_equal(config_get_int(&v, CFG_API_POOL_SIZE), 4);
    assert_int_equal(config_get_bool(&v, CFG_MODE), 0); /* not a bool value */
    /* Unset required key yields the empty string. */
    assert_string_equal(config_get_str(&v, CFG_BOT_TOKEN), "");
}

static void test_load_file_and_override_precedence(void **state)
{
    (void)state;
    char *path = NULL;
    assert_int_equal(write_temp("bot_token = \"from_file\"\n[api]\npool_size = 9\n", &path), 0);

    config_values_t v;
    assert_int_equal(config_load(path, NULL, &v), 0);
    assert_string_equal(config_get_str(&v, CFG_BOT_TOKEN), "from_file");
    assert_int_equal(config_get_int(&v, CFG_API_POOL_SIZE), 9);

    /* CLI override beats the file. */
    cfg_override_t ov[] = {{.id = CFG_API_POOL_SIZE, .value = "12"},
                           {.id = CFG_BOT_TOKEN, .value = "from_cli"},
                           {.value = NULL}};
    assert_int_equal(config_load(path, ov, &v), 0);
    assert_int_equal(config_get_int(&v, CFG_API_POOL_SIZE), 12);
    assert_string_equal(config_get_str(&v, CFG_BOT_TOKEN), "from_cli");
    free(path);
}

static void test_load_rejects_bad_enum(void **state)
{
    (void)state;
    char *path = NULL;
    assert_int_equal(write_temp("mode = \"bogus\"\n", &path), 0);
    config_values_t v;
    assert_int_not_equal(config_load(path, NULL, &v), 0);
    free(path);
}

static void test_getters_null_and_bounds(void **state)
{
    (void)state;
    assert_string_equal(config_get_str(NULL, CFG_MODE), "");
    assert_int_equal(config_get_int(NULL, CFG_MODE), 0);
    assert_int_equal(config_get_bool(NULL, CFG_MODE), 0);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_keys_and_env_unique),
        cmocka_unit_test(test_enum_keys_have_values),
        cmocka_unit_test(test_required_bot_token),
        cmocka_unit_test(test_lookup_by_key_and_env),
        cmocka_unit_test(test_sample_config_is_valid),
        cmocka_unit_test(test_unknown_key_rejected),
        cmocka_unit_test(test_bad_enum_rejected),
        cmocka_unit_test(test_missing_required_rejected),
        cmocka_unit_test(test_load_defaults_and_getters),
        cmocka_unit_test(test_load_file_and_override_precedence),
        cmocka_unit_test(test_load_rejects_bad_enum),
        cmocka_unit_test(test_getters_null_and_bounds),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
