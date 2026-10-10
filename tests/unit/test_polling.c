/* test_polling.c - getUpdates offset logic and the poll loop (A5.1).
 *
 * The offset helper is pure; the loop is driven with a fake tg_api_ops_t so no
 * network is needed. Verifies the offset advance, the allowed_updates body and
 * that a failed poll never advances the offset.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "api_methods.h"
#include "tg/api_pool.h"
#include "tg/errors.h"
#include "tg/polling.h"
#include "tg/tg_api.h"
#include "tg/util.h"

/* --- Offset helper --------------------------------------------------------- */

static void test_offset_advances_past_max(void **state)
{
    (void)state;
    int64_t offset = 0;
    unsigned count = 0;
    const char *json = "{\"ok\":true,\"result\":[{\"update_id\":10},{\"update_id\":12},"
                       "{\"update_id\":11}]}";
    assert_int_equal(poller_advance_offset(&offset, json, strlen(json), &count), 0);
    assert_int_equal(count, 3);
    assert_int_equal(offset, 13);
}

static void test_offset_empty_batch_unchanged(void **state)
{
    (void)state;
    int64_t offset = 41;
    unsigned count = 99;
    const char *json = "{\"ok\":true,\"result\":[]}";
    assert_int_equal(poller_advance_offset(&offset, json, strlen(json), &count), 0);
    assert_int_equal(count, 0);
    assert_int_equal(offset, 41);
}

static void test_offset_duplicate_ids_do_not_regress(void **state)
{
    (void)state;
    int64_t offset = 100;
    unsigned count = 0;
    /* A stale batch below the offset must not move it backwards. */
    const char *json = "{\"ok\":true,\"result\":[{\"update_id\":50},{\"update_id\":60}]}";
    assert_int_equal(poller_advance_offset(&offset, json, strlen(json), &count), 0);
    assert_int_equal(offset, 100);
}

static void test_offset_error_envelope_unchanged(void **state)
{
    (void)state;
    int64_t offset = 7;
    unsigned count = 5;
    const char *json = "{\"ok\":false,\"error_code\":409,\"description\":\"Conflict\"}";
    assert_int_equal(poller_advance_offset(&offset, json, strlen(json), &count), -ERR_PARSE);
    assert_int_equal(offset, 7);

    assert_int_equal(poller_advance_offset(&offset, "not json", 8, &count), -ERR_PARSE);
    assert_int_equal(poller_advance_offset(NULL, json, strlen(json), &count), -EINVAL);
}

/* --- Body builder ---------------------------------------------------------- */

static void test_get_updates_body(void **state)
{
    (void)state;
    char body[256];
    size_t len = 0;
    assert_int_equal(tg_api_build_get_updates_body(body, sizeof(body), 42, 30, &len), 0);
    assert_non_null(strstr(body, "\"offset\":42"));
    assert_non_null(strstr(body, "\"timeout\":30"));
    assert_non_null(strstr(body, "\"message\""));
    assert_non_null(strstr(body, "\"my_chat_member\""));
    assert_int_equal(len, strlen(body));

    char small[8];
    assert_int_equal(tg_api_build_get_updates_body(small, sizeof(small), 0, 30, &len), -ENOSPC);
}

/* --- Poll loop over a fake seam -------------------------------------------- */

typedef struct {
    int calls;
    int64_t last_offset;
    api_result_t result; /**< Delivered on the next fake_deliver(). */
    tg_api_cb pending_cb;
    void *pending_ctx;
} fake_seam_t;

/* The fake does NOT invoke the callback inline: it stores it so the test can
 * deliver the reply explicitly, matching the real pool's async completion. */
static int fake_request(void *ctx, method_id_t method, const char *body, size_t body_len,
                        int64_t chat_id, uint64_t deadline_ms, tg_api_cb cb, void *user)
{
    (void)method;
    (void)chat_id;
    (void)deadline_ms;
    (void)body_len;
    fake_seam_t *f = ctx;
    f->calls++;
    /* Recover the offset from the JSON body. */
    const char *p = strstr(body, "\"offset\":");
    f->last_offset = (p != NULL) ? strtoll(p + 9, NULL, 10) : -1;
    f->pending_cb = cb;
    f->pending_ctx = user;
    return 0;
}

static void fake_deliver(fake_seam_t *f)
{
    tg_api_cb cb = f->pending_cb;
    f->pending_cb = NULL;
    cb(&f->result, f->pending_ctx);
}

static void test_poll_loop_advances_and_rearms(void **state)
{
    (void)state;
    fake_seam_t f = {0};
    const char *json = "{\"ok\":true,\"result\":[{\"update_id\":5}]}";
    f.result = (api_result_t){.status = 0, .body = json, .body_len = strlen(json)};

    tg_api_ops_t ops = {.request = fake_request, .ctx = &f};
    poller_config_t cfg = {.ops = &ops, .clock = &util_clock_ops_real, .timeout_s = 30};
    poller_t *p = poller_create(&cfg);
    assert_non_null(p);

    assert_int_equal(poller_start(p), 0);
    assert_int_equal(f.calls, 1);

    /* Deliver the reply: the offset advances to 6 and the poller re-arms. */
    fake_deliver(&f);
    assert_int_equal(poller_offset(p), 6);
    assert_int_equal(f.calls, 2);
    assert_int_equal(f.last_offset, 6);

    /* After stop, a late reply must not re-arm. */
    poller_stop(p);
    fake_deliver(&f);
    assert_int_equal(f.calls, 2);
    poller_destroy(p);
}

static void test_poll_loop_error_keeps_offset(void **state)
{
    (void)state;
    fake_seam_t f = {0};
    f.result = (api_result_t){.status = -ERR_NET};

    tg_api_ops_t ops = {.request = fake_request, .ctx = &f};
    poller_config_t cfg = {.ops = &ops, .clock = &util_clock_ops_real, .timeout_s = 30};
    poller_t *p = poller_create(&cfg);
    assert_non_null(p);

    assert_int_equal(poller_start(p), 0);
    fake_deliver(&f);
    assert_int_equal(poller_offset(p), 0); /* never advanced on failure */
    assert_int_equal(f.calls, 2);          /* re-armed anyway */
    poller_stop(p);
    poller_destroy(p);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_offset_advances_past_max),
        cmocka_unit_test(test_offset_empty_batch_unchanged),
        cmocka_unit_test(test_offset_duplicate_ids_do_not_regress),
        cmocka_unit_test(test_offset_error_envelope_unchanged),
        cmocka_unit_test(test_get_updates_body),
        cmocka_unit_test(test_poll_loop_advances_and_rearms),
        cmocka_unit_test(test_poll_loop_error_keeps_offset),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
