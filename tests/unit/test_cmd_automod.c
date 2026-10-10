/* test_cmd_automod.c - automod configuration commands (SPEC section 5.2).
 *
 * Drives /lock, /unlock, /blacklist, /antiflood and /captcha through a real
 * reference-counted invocation over fake API/DB seams: the lock bitmask parsing
 * (multiple types, unknown rejection), the blacklist add/del/list argument
 * handling and the antiflood/captcha argument validation. No network, no SQLite.
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

#include "api_methods.h"
#include "commands.h"
#include "tables.h"
#include "tg/api_pool.h"
#include "tg/automod.h"
#include "tg/cmd_ctx.h"
#include "tg/db.h"
#include "tg/tg_api.h"
#include "tg/tg_types.h"
#include "tg/util.h"

/* --- Fakes ----------------------------------------------------------------- */

typedef struct {
    int replies;
    char last_reply[512];
    int db_submits;
    db_job_t last_job;
    int invalidated;
} fake_t;

static int fake_request(void *ctx, method_id_t method, const char *body, size_t body_len,
                        int64_t chat_id, uint64_t deadline_ms, tg_api_cb cb, void *user)
{
    (void)ctx;
    (void)method;
    (void)body;
    (void)body_len;
    (void)chat_id;
    (void)deadline_ms;
    if (cb != NULL) {
        api_result_t res = {0};
        cb(&res, user);
    }
    return 0;
}

static int fake_submit(void *ctx, const db_job_t *job, db_cb cb, void *user)
{
    fake_t *f = ctx;
    f->db_submits++;
    f->last_job = *job;
    if (cb != NULL) {
        db_result_t res = {0};
        cb(&res, user);
    }
    return 0;
}

static void fake_invalidate(void *impl, int64_t chat_id)
{
    (void)chat_id;
    fake_t *f = impl;
    f->invalidated++;
}

/** @brief Capture the reply text (the invocation's reply hook is wrapped). */
static fake_t g_capture;

static int capture_reply(struct cmd_ctx *ctx, const char *html, int64_t reply_to)
{
    (void)ctx;
    (void)reply_to;
    g_capture.replies++;
    (void)snprintf(g_capture.last_reply, sizeof(g_capture.last_reply), "%s", html);
    return 0;
}

static cmd_call_t *make_call(const tg_api_ops_t *api, const db_ops_t *db,
                             const wall_clock_ops_t *wall, const char *args,
                             const chat_settings_t *settings)
{
    cmd_call_t *call = cmd_call_create(api, db, wall);
    assert_non_null(call);
    struct cmd_ctx *ctx = cmd_call_ctx(call);
    tg_message_t *msg = cmd_call_message(call);
    msg->chat.id = -100;
    (void)snprintf(msg->chat.type, sizeof(msg->chat.type), "supergroup");
    msg->message_id = 10;
    msg->from.id = 200;
    ctx->chat_id = -100;
    ctx->user_id = 200;
    ctx->bot_id = 999;
    ctx->args = args;
    ctx->is_group = 1;
    ctx->role = ROLE_ADMIN;
    /* Replace the reply hook so the test captures the text; keep the real
     * db_submit and invalidate hooks. */
    ctx->reply = capture_reply;
    if (settings != NULL) {
        (void)cmd_ctx_set_settings(ctx, settings);
    }
    return call;
}

static void reset_capture(void)
{
    memset(&g_capture, 0, sizeof(g_capture));
}

/* --- /lock ----------------------------------------------------------------- */

static void test_lock_sets_bitmask(void **state)
{
    (void)state;
    reset_capture();
    chat_settings_t s = {0};
    s.locks = LOCK_MEDIA;
    db_ops_t dbops = {.submit = fake_submit, .ctx = &g_capture};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = NULL};
    cmd_call_t *call = make_call(&apiops, &dbops, &util_wall_clock_real, "links stickers", &s);
    struct cmd_ctx *ctx = cmd_call_ctx(call);
    cmd_call_set_settings_invalidator(call, fake_invalidate, &g_capture);
    assert_int_equal(cmd_lock(ctx), 0);
    assert_int_equal(g_capture.db_submits, 1);
    assert_int_equal(g_capture.last_job.kind, DB_JOB_SETTINGS_SET);
    assert_int_equal(g_capture.last_job.settings_mask, DB_SET_LOCKS);
    assert_int_equal(g_capture.last_job.value, (int64_t)(LOCK_MEDIA | LOCK_LINKS | LOCK_STICKERS));
    assert_int_equal(g_capture.invalidated, 1);
    cmd_call_release(call);
}

static void test_lock_unknown_type_rejected(void **state)
{
    (void)state;
    reset_capture();
    db_ops_t dbops = {.submit = fake_submit, .ctx = &g_capture};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = NULL};
    cmd_call_t *call = make_call(&apiops, &dbops, &util_wall_clock_real, "links bogus", NULL);
    assert_int_equal(cmd_lock(cmd_call_ctx(call)), 0);
    assert_int_equal(g_capture.db_submits, 0);
    assert_non_null(strstr(g_capture.last_reply, "Unknown lock type"));
    cmd_call_release(call);
}

static void test_lock_no_args_shows_usage(void **state)
{
    (void)state;
    reset_capture();
    db_ops_t dbops = {.submit = fake_submit, .ctx = &g_capture};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = NULL};
    cmd_call_t *call = make_call(&apiops, &dbops, &util_wall_clock_real, "", NULL);
    assert_int_equal(cmd_lock(cmd_call_ctx(call)), 0);
    assert_int_equal(g_capture.db_submits, 0);
    assert_non_null(strstr(g_capture.last_reply, "Usage"));
    cmd_call_release(call);
}

static void test_unlock_clears_bit(void **state)
{
    (void)state;
    reset_capture();
    chat_settings_t s = {0};
    s.locks = (unsigned)(LOCK_MEDIA | LOCK_LINKS);
    db_ops_t dbops = {.submit = fake_submit, .ctx = &g_capture};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = NULL};
    cmd_call_t *call = make_call(&apiops, &dbops, &util_wall_clock_real, "links", &s);
    assert_int_equal(cmd_unlock(cmd_call_ctx(call)), 0);
    assert_int_equal(g_capture.last_job.value, LOCK_MEDIA);
    cmd_call_release(call);
}

static void test_lock_comma_separated(void **state)
{
    (void)state;
    reset_capture();
    db_ops_t dbops = {.submit = fake_submit, .ctx = &g_capture};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = NULL};
    cmd_call_t *call = make_call(&apiops, &dbops, &util_wall_clock_real, "forwards, polls", NULL);
    assert_int_equal(cmd_lock(cmd_call_ctx(call)), 0);
    assert_int_equal(g_capture.last_job.value, (int64_t)(LOCK_FORWARDS | LOCK_POLLS));
    cmd_call_release(call);
}

/* --- /blacklist ------------------------------------------------------------ */

static void test_blacklist_add_plain(void **state)
{
    (void)state;
    reset_capture();
    db_ops_t dbops = {.submit = fake_submit, .ctx = &g_capture};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = NULL};
    cmd_call_t *call = make_call(&apiops, &dbops, &util_wall_clock_real, "add scam", NULL);
    assert_int_equal(cmd_blacklist(cmd_call_ctx(call)), 0);
    assert_int_equal(g_capture.last_job.kind, DB_JOB_BLACKLIST_ADD);
    assert_string_equal(g_capture.last_job.pattern, "scam");
    assert_int_equal(g_capture.last_job.value, 0);
    cmd_call_release(call);
}

static void test_blacklist_add_regex(void **state)
{
    (void)state;
    reset_capture();
    db_ops_t dbops = {.submit = fake_submit, .ctx = &g_capture};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = NULL};
    cmd_call_t *call = make_call(&apiops, &dbops, &util_wall_clock_real, "add /fr[e3]+e/", NULL);
    assert_int_equal(cmd_blacklist(cmd_call_ctx(call)), 0);
    assert_int_equal(g_capture.last_job.kind, DB_JOB_BLACKLIST_ADD);
    assert_string_equal(g_capture.last_job.pattern, "fr[e3]+e");
    assert_int_equal(g_capture.last_job.value, 1);
    cmd_call_release(call);
}

static void test_blacklist_del(void **state)
{
    (void)state;
    reset_capture();
    db_ops_t dbops = {.submit = fake_submit, .ctx = &g_capture};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = NULL};
    cmd_call_t *call = make_call(&apiops, &dbops, &util_wall_clock_real, "del scam", NULL);
    assert_int_equal(cmd_blacklist(cmd_call_ctx(call)), 0);
    assert_int_equal(g_capture.last_job.kind, DB_JOB_BLACKLIST_DEL);
    cmd_call_release(call);
}

static void test_blacklist_list(void **state)
{
    (void)state;
    reset_capture();
    db_ops_t dbops = {.submit = fake_submit, .ctx = &g_capture};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = NULL};
    cmd_call_t *call = make_call(&apiops, &dbops, &util_wall_clock_real, "list", NULL);
    assert_int_equal(cmd_blacklist(cmd_call_ctx(call)), 0);
    assert_int_equal(g_capture.last_job.kind, DB_JOB_BLACKLIST_LIST);
    cmd_call_release(call);
}

static void test_blacklist_usage(void **state)
{
    (void)state;
    reset_capture();
    db_ops_t dbops = {.submit = fake_submit, .ctx = &g_capture};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = NULL};
    cmd_call_t *call = make_call(&apiops, &dbops, &util_wall_clock_real, "", NULL);
    assert_int_equal(cmd_blacklist(cmd_call_ctx(call)), 0);
    assert_int_equal(g_capture.db_submits, 0);
    assert_non_null(strstr(g_capture.last_reply, "Usage"));
    cmd_call_release(call);
}

/* --- /antiflood ------------------------------------------------------------ */

static void test_antiflood_sets_threshold_and_window(void **state)
{
    (void)state;
    reset_capture();
    db_ops_t dbops = {.submit = fake_submit, .ctx = &g_capture};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = NULL};
    cmd_call_t *call = make_call(&apiops, &dbops, &util_wall_clock_real, "5 10", NULL);
    assert_int_equal(cmd_antiflood(cmd_call_ctx(call)), 0);
    assert_int_equal(g_capture.db_submits, 2); /* threshold then window */
    cmd_call_release(call);
}

static void test_antiflood_off(void **state)
{
    (void)state;
    reset_capture();
    db_ops_t dbops = {.submit = fake_submit, .ctx = &g_capture};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = NULL};
    cmd_call_t *call = make_call(&apiops, &dbops, &util_wall_clock_real, "off", NULL);
    assert_int_equal(cmd_antiflood(cmd_call_ctx(call)), 0);
    assert_int_equal(g_capture.last_job.settings_mask, DB_SET_FLOOD_LIMIT);
    assert_int_equal(g_capture.last_job.value, 0);
    cmd_call_release(call);
}

static void test_antiflood_bad_args(void **state)
{
    (void)state;
    reset_capture();
    db_ops_t dbops = {.submit = fake_submit, .ctx = &g_capture};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = NULL};
    cmd_call_t *call = make_call(&apiops, &dbops, &util_wall_clock_real, "5", NULL);
    assert_int_equal(cmd_antiflood(cmd_call_ctx(call)), 0);
    assert_int_equal(g_capture.db_submits, 0);
    assert_non_null(strstr(g_capture.last_reply, "Usage"));
    cmd_call_release(call);
}

/* --- /captcha -------------------------------------------------------------- */

static void test_captcha_on_off(void **state)
{
    (void)state;
    reset_capture();
    db_ops_t dbops = {.submit = fake_submit, .ctx = &g_capture};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = NULL};
    cmd_call_t *call = make_call(&apiops, &dbops, &util_wall_clock_real, "on", NULL);
    assert_int_equal(cmd_captcha(cmd_call_ctx(call)), 0);
    assert_int_equal(g_capture.last_job.settings_mask, DB_SET_CAPTCHA_ON);
    assert_int_equal(g_capture.last_job.value, 1);
    cmd_call_release(call);

    reset_capture();
    cmd_call_t *call2 = make_call(&apiops, &dbops, &util_wall_clock_real, "off", NULL);
    assert_int_equal(cmd_captcha(cmd_call_ctx(call2)), 0);
    assert_int_equal(g_capture.last_job.value, 0);
    cmd_call_release(call2);
}

static void test_captcha_usage(void **state)
{
    (void)state;
    reset_capture();
    db_ops_t dbops = {.submit = fake_submit, .ctx = &g_capture};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = NULL};
    cmd_call_t *call = make_call(&apiops, &dbops, &util_wall_clock_real, "maybe", NULL);
    assert_int_equal(cmd_captcha(cmd_call_ctx(call)), 0);
    assert_int_equal(g_capture.db_submits, 0);
    assert_non_null(strstr(g_capture.last_reply, "Usage"));
    cmd_call_release(call);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_lock_sets_bitmask),
        cmocka_unit_test(test_lock_unknown_type_rejected),
        cmocka_unit_test(test_lock_no_args_shows_usage),
        cmocka_unit_test(test_unlock_clears_bit),
        cmocka_unit_test(test_lock_comma_separated),
        cmocka_unit_test(test_blacklist_add_plain),
        cmocka_unit_test(test_blacklist_add_regex),
        cmocka_unit_test(test_blacklist_del),
        cmocka_unit_test(test_blacklist_list),
        cmocka_unit_test(test_blacklist_usage),
        cmocka_unit_test(test_antiflood_sets_threshold_and_window),
        cmocka_unit_test(test_antiflood_off),
        cmocka_unit_test(test_antiflood_bad_args),
        cmocka_unit_test(test_captcha_on_off),
        cmocka_unit_test(test_captcha_usage),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
