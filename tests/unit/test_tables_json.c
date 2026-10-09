/* test_tables_json.c - deterministic SSOT JSON dumps (SPEC-ADDENDUM-SSOT S5, S9.4).
 *
 * Verifies each dump is valid JSON, has the expected shape, and is byte-identical
 * across two runs (the determinism guarantee).
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <stdio.h>
#include <string.h>

#include <cmocka.h>
#include <yyjson.h>

#include "tables_json.h"

/* Render a dump into a static buffer; valid until the next call. */
static const char *dump_to_string(int (*fn)(FILE *))
{
    static char buf[65536];
    FILE *f = tmpfile();
    if (f == NULL) {
        return "";
    }
    int rc = fn(f);
    if (rc != 0) {
        (void)fclose(f);
        return "";
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        return "";
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    (void)fclose(f);
    return buf;
}

static void test_commands_json(void **state)
{
    (void)state;
    const char *s = dump_to_string(tables_dump_commands_json);
    yyjson_doc *doc = yyjson_read(s, strlen(s), 0);
    assert_non_null(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert_true(yyjson_is_arr(root));
    yyjson_val *first = yyjson_arr_get_first(root);
    assert_non_null(first);
    assert_non_null(yyjson_obj_get(first, "id"));
    assert_non_null(yyjson_obj_get(first, "min_role"));
    assert_non_null(yyjson_obj_get(first, "hidden"));
    yyjson_doc_free(doc);
}

static void test_config_json(void **state)
{
    (void)state;
    const char *s = dump_to_string(tables_dump_config_json);
    yyjson_doc *doc = yyjson_read(s, strlen(s), 0);
    assert_non_null(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert_true(yyjson_is_arr(root));
    yyjson_val *first = yyjson_arr_get_first(root);
    assert_non_null(yyjson_obj_get(first, "key"));
    assert_non_null(yyjson_obj_get(first, "required"));
    assert_non_null(yyjson_obj_get(first, "enum_values"));
    yyjson_doc_free(doc);
}

static void test_all_tables_json(void **state)
{
    (void)state;
    const char *s = dump_to_string(tables_dump_all_json);
    yyjson_doc *doc = yyjson_read(s, strlen(s), 0);
    assert_non_null(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert_true(yyjson_is_obj(root));
    const char *keys[] = {"commands", "config", "locks", "api_methods", "errors", "metrics"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        yyjson_val *arr = yyjson_obj_get(root, keys[i]);
        assert_non_null(arr);
        assert_true(yyjson_is_arr(arr));
    }
    yyjson_doc_free(doc);
}

static void test_determinism(void **state)
{
    (void)state;
    /* Copy the first dump before the second call overwrites the shared buffer. */
    char first[65536];
    (void)snprintf(first, sizeof(first), "%s", dump_to_string(tables_dump_all_json));
    const char *second = dump_to_string(tables_dump_all_json);
    assert_string_equal(first, second);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_commands_json),
        cmocka_unit_test(test_config_json),
        cmocka_unit_test(test_all_tables_json),
        cmocka_unit_test(test_determinism),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
