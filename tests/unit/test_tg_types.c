/* test_tg_types.c - update parsing (SPEC-ADDENDUM-TOOLCHAIN A5.1).
 *
 * Pure logic, no network: feeds recorded-shape JSON payloads into the parser and
 * checks the field mapping for every update kind, plus missing/null/extra fields
 * and huge int64 ids (negative supergroup chat ids).
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

#include "tg/errors.h"
#include "tg/tg_types.h"

static void test_message_basic(void **state)
{
    (void)state;
    const char *json =
        "{\"update_id\":100,\"message\":{\"message_id\":7,\"date\":1700000000,"
        "\"from\":{\"id\":42,\"is_bot\":false,\"first_name\":\"Ada\",\"username\":\"ada\"},"
        "\"chat\":{\"id\":-1001234567890,\"type\":\"supergroup\",\"title\":\"Testers\"},"
        "\"text\":\"hello world\"}}";
    tg_update_t u;
    assert_int_equal(tg_update_parse(json, strlen(json), &u), 0);
    assert_int_equal(u.update_id, 100);
    assert_int_equal(u.kind, TG_UPD_MESSAGE);
    assert_int_equal(u.message.message_id, 7);
    assert_int_equal(u.message.date, 1700000000);
    assert_int_equal(u.message.from.id, 42);
    assert_false(u.message.from.is_bot);
    assert_string_equal(u.message.from.first_name, "Ada");
    assert_string_equal(u.message.from.username, "ada");
    assert_int_equal(u.message.chat.id, -1001234567890LL);
    assert_string_equal(u.message.chat.type, "supergroup");
    assert_string_equal(u.message.chat.title, "Testers");
    assert_string_equal(u.message.text, "hello world");
    assert_false(u.message.is_service);
}

static void test_edited_message_kind(void **state)
{
    (void)state;
    const char *json =
        "{\"update_id\":1,\"edited_message\":{\"message_id\":2,\"text\":\"edited\"}}";
    tg_update_t u;
    assert_int_equal(tg_update_parse(json, strlen(json), &u), 0);
    assert_int_equal(u.kind, TG_UPD_EDITED_MESSAGE);
    assert_string_equal(u.message.text, "edited");
}

static void test_reply_captures_target(void **state)
{
    (void)state;
    const char *json =
        "{\"update_id\":2,\"message\":{\"message_id\":9,\"text\":\"/ban\","
        "\"reply_to_message\":{\"message_id\":8,\"from\":{\"id\":555,\"first_name\":\"Bob\","
        "\"username\":\"bob\"}}}}";
    tg_update_t u;
    assert_int_equal(tg_update_parse(json, strlen(json), &u), 0);
    assert_true(u.message.has_reply);
    assert_int_equal(u.message.reply_to_message_id, 8);
    assert_int_equal(u.message.reply_from.id, 555);
    assert_string_equal(u.message.reply_from.username, "bob");
}

static void test_service_message_has_no_sender(void **state)
{
    (void)state;
    const char *json =
        "{\"update_id\":3,\"message\":{\"message_id\":10,\"chat\":{\"id\":-5,\"type\":\"group\"},"
        "\"new_chat_members\":[{\"id\":11,\"is_bot\":false,\"first_name\":\"New\"}]}}";
    tg_update_t u;
    assert_int_equal(tg_update_parse(json, strlen(json), &u), 0);
    assert_true(u.message.is_service);
    assert_int_equal(u.message.new_member_count, 1);
    assert_int_equal(u.message.new_members[0].id, 11);
}

static void test_callback_query(void **state)
{
    (void)state;
    const char *json = "{\"update_id\":4,\"callback_query\":{\"id\":\"cb-1\","
                       "\"from\":{\"id\":77,\"first_name\":\"Eve\"},\"data\":\"cap:-5:77:abcd\"}}";
    tg_update_t u;
    assert_int_equal(tg_update_parse(json, strlen(json), &u), 0);
    assert_int_equal(u.kind, TG_UPD_CALLBACK_QUERY);
    assert_string_equal(u.callback_id, "cb-1");
    assert_int_equal(u.callback_from.id, 77);
    assert_string_equal(u.callback_data, "cap:-5:77:abcd");
}

static void test_chat_member_statuses(void **state)
{
    (void)state;
    const char *json =
        "{\"update_id\":5,\"chat_member\":{\"chat\":{\"id\":-100,\"type\":\"supergroup\"},"
        "\"from\":{\"id\":1,\"first_name\":\"Admin\"},"
        "\"old_chat_member\":{\"status\":\"member\"},"
        "\"new_chat_member\":{\"user\":{\"id\":2,\"first_name\":\"Jo\"},"
        "\"status\":\"administrator\"}}}";
    tg_update_t u;
    assert_int_equal(tg_update_parse(json, strlen(json), &u), 0);
    assert_int_equal(u.kind, TG_UPD_CHAT_MEMBER);
    assert_int_equal(u.member_chat.id, -100);
    assert_int_equal(u.member_from.id, 1);
    assert_int_equal(u.member_user.id, 2);
    assert_string_equal(u.old_status, "member");
    assert_string_equal(u.new_status, "administrator");
}

static void test_my_chat_member(void **state)
{
    (void)state;
    const char *json =
        "{\"update_id\":6,\"my_chat_member\":{\"chat\":{\"id\":-100,\"type\":\"supergroup\"},"
        "\"new_chat_member\":{\"user\":{\"id\":999,\"is_bot\":true},\"status\":\"member\"}}}";
    tg_update_t u;
    assert_int_equal(tg_update_parse(json, strlen(json), &u), 0);
    assert_int_equal(u.kind, TG_UPD_MY_CHAT_MEMBER);
    assert_int_equal(u.member_user.id, 999);
    assert_string_equal(u.new_status, "member");
}

static void test_unknown_and_malformed(void **state)
{
    (void)state;
    tg_update_t u;
    const char *unknown = "{\"update_id\":7,\"channel_post\":{\"message_id\":1}}";
    assert_int_equal(tg_update_parse(unknown, strlen(unknown), &u), 0);
    assert_int_equal(u.kind, TG_UPD_UNKNOWN);
    assert_int_equal(u.update_id, 7);

    assert_int_equal(tg_update_parse("not json", 8, &u), -ERR_PARSE);
    assert_int_equal(tg_update_parse("[1,2]", 5, &u), -ERR_PARSE);
    assert_int_equal(tg_update_parse(NULL, 0, &u), -EINVAL);
    assert_int_equal(tg_update_parse("{}", 2, NULL), -EINVAL);
}

static void test_missing_null_and_extra_fields(void **state)
{
    (void)state;
    const char *json = "{\"update_id\":8,\"message\":{\"message_id\":1,\"text\":null,\"from\":null,"
                       "\"chat\":{\"id\":-1,\"type\":\"group\"},\"extra\":{\"nested\":true}},"
                       "\"another_extra\":42}";
    tg_update_t u;
    assert_int_equal(tg_update_parse(json, strlen(json), &u), 0);
    assert_string_equal(u.message.text, "");
    assert_int_equal(u.message.from.id, 0);
    assert_string_equal(u.message.chat.type, "group");
}

static void test_text_truncated_to_cap(void **state)
{
    (void)state;
    /* 5000 'a's must be truncated to the 4096-byte text buffer, NUL-terminated. */
    char json[8192];
    int n = snprintf(json, sizeof(json), "{\"update_id\":9,\"message\":{\"text\":\"");
    for (int i = 0; i < 5000; ++i) {
        json[n++] = 'a';
    }
    n += snprintf(json + n, sizeof(json) - (size_t)n, "\"}}");
    tg_update_t u;
    assert_int_equal(tg_update_parse(json, (size_t)n, &u), 0);
    assert_int_equal(strlen(u.message.text), 4096);
}

/* --- Batch parsing --------------------------------------------------------- */

static int count_cb(const tg_update_t *u, void *ctx)
{
    (void)u;
    int *n = ctx;
    (*n)++;
    return 0;
}

static int stop_after_one_cb(const tg_update_t *u, void *ctx)
{
    (void)u;
    int *n = ctx;
    (*n)++;
    return 1; /* stop iteration */
}

static void test_updates_batch(void **state)
{
    (void)state;
    const char *json = "{\"ok\":true,\"result\":[{\"update_id\":1,\"message\":{\"message_id\":1}},"
                       "{\"update_id\":2,\"message\":{\"message_id\":2}},{\"update_id\":3}]}";
    int seen = 0;
    unsigned count = 0;
    assert_int_equal(tg_updates_parse(json, strlen(json), count_cb, &seen, &count), 0);
    assert_int_equal(count, 3);
    assert_int_equal(seen, 3);
}

static void test_updates_stop_early(void **state)
{
    (void)state;
    const char *json =
        "{\"ok\":true,\"result\":[{\"update_id\":1},{\"update_id\":2},{\"update_id\":3}]}";
    int seen = 0;
    unsigned count = 0;
    assert_int_equal(tg_updates_parse(json, strlen(json), stop_after_one_cb, &seen, &count), 0);
    assert_int_equal(seen, 1);
    assert_int_equal(count, 1);
}

static void test_updates_empty_and_error_envelope(void **state)
{
    (void)state;
    unsigned count = 99;
    int seen = 0;
    const char *empty = "{\"ok\":true,\"result\":[]}";
    assert_int_equal(tg_updates_parse(empty, strlen(empty), count_cb, &seen, &count), 0);
    assert_int_equal(count, 0);
    assert_int_equal(seen, 0);

    const char *err = "{\"ok\":false,\"error_code\":409,\"description\":\"Conflict\"}";
    assert_int_equal(tg_updates_parse(err, strlen(err), count_cb, NULL, &count), -ERR_PARSE);
    assert_int_equal(tg_updates_parse("garbage", 7, count_cb, NULL, &count), -ERR_PARSE);
    assert_int_equal(tg_updates_parse(NULL, 0, count_cb, NULL, &count), -EINVAL);
    assert_int_equal(tg_updates_parse("{}", 2, NULL, NULL, &count), -EINVAL);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_message_basic),
        cmocka_unit_test(test_edited_message_kind),
        cmocka_unit_test(test_reply_captures_target),
        cmocka_unit_test(test_service_message_has_no_sender),
        cmocka_unit_test(test_callback_query),
        cmocka_unit_test(test_chat_member_statuses),
        cmocka_unit_test(test_my_chat_member),
        cmocka_unit_test(test_unknown_and_malformed),
        cmocka_unit_test(test_missing_null_and_extra_fields),
        cmocka_unit_test(test_text_truncated_to_cap),
        cmocka_unit_test(test_updates_batch),
        cmocka_unit_test(test_updates_stop_early),
        cmocka_unit_test(test_updates_empty_and_error_envelope),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
