/* test_cmd_msg.c - message-management handlers over a fake API seam (A5.1).
 *
 * Drives /del, /pin, /unpin and /purge through a fake tg_api_ops_t that records
 * each call, checking the two-call /del flow, the silent pin, the unpin, and the
 * deleteMessages batching of /purge. No network.
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
#include "tg/cmd_ctx.h"
#include "tg/tg_api.h"
#include "tg/tg_types.h"
#include "tg/util.h"

#define FAKE_QUEUE 32u

typedef struct {
    int calls;
    method_id_t last_method;
    char last_body[8192];
    struct {
        tg_api_cb cb;
        void *user;
    } pending[FAKE_QUEUE];
    size_t pending_len;
} fake_api_t;

static int fake_request(void *ctx, method_id_t method, const char *body, size_t body_len,
                        int64_t chat_id, uint64_t deadline_ms, tg_api_cb cb, void *user)
{
    (void)chat_id;
    (void)deadline_ms;
    fake_api_t *f = ctx;
    f->calls++;
    f->last_method = method;
    size_t n = body_len < sizeof(f->last_body) - 1u ? body_len : sizeof(f->last_body) - 1u;
    if (body != NULL) {
        memcpy(f->last_body, body, n);
    }
    f->last_body[n] = '\0';
    if (f->pending_len >= FAKE_QUEUE) {
        return -ENOSPC; /* the test never queues this many; guard the bound */
    }
    f->pending[f->pending_len].cb = cb;
    f->pending[f->pending_len].user = user;
    f->pending_len++;
    return 0;
}

static void fake_drain(fake_api_t *f)
{
    while (f->pending_len > 0) {
        tg_api_cb cb = f->pending[0].cb;
        void *user = f->pending[0].user;
        for (size_t i = 1; i < f->pending_len; ++i) {
            f->pending[i - 1] = f->pending[i];
        }
        f->pending_len--;
        if (cb != NULL) {
            api_result_t res = {0};
            cb(&res, user);
        }
    }
}

static int64_t fake_now_unix(void *ctx)
{
    return *(int64_t *)ctx;
}

/** @brief Build a call whose message replies to message id 41. */
static cmd_call_t *make_call(const tg_api_ops_t *api, const wall_clock_ops_t *wall,
                             const char *cmd_name, int64_t reply_to)
{
    cmd_call_t *call = cmd_call_create(api, NULL, wall);
    assert_non_null(call);
    struct cmd_ctx *ctx = cmd_call_ctx(call);
    tg_message_t *msg = cmd_call_message(call);
    msg->chat.id = -100;
    (void)snprintf(msg->chat.type, sizeof(msg->chat.type), "supergroup");
    msg->message_id = 42;
    msg->from.id = 200;
    if (reply_to != 0) {
        msg->has_reply = 1;
        msg->reply_to_message_id = reply_to;
    }
    ctx->cmd = cmd_lookup(cmd_name);
    ctx->chat_id = -100;
    ctx->user_id = 200;
    ctx->bot_id = 999;
    ctx->is_group = 1;
    ctx->role = ROLE_ADMIN;
    ctx->args = "";
    return call;
}

static void test_del_issues_two_delete_message_calls(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, &wall, "del", 41);
    assert_int_equal(cmd_del(cmd_call_ctx(call)), 0);
    assert_int_equal(f.calls, 2);
    assert_int_equal(f.last_method, METHOD_DELETE_MESSAGE);
    cmd_call_release(call);
    fake_drain(&f);
}

static void test_del_requires_reply(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, &wall, "del", 0);
    assert_int_equal(cmd_del(cmd_call_ctx(call)), 0);
    assert_int_equal(f.calls, 1); /* only the "needs a reply" message */
    assert_int_equal(f.last_method, METHOD_SEND_MESSAGE);
    cmd_call_release(call);
    fake_drain(&f);
}

static void test_pin_silent(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, &wall, "pin", 41);
    assert_int_equal(cmd_pin(cmd_call_ctx(call)), 0);
    assert_int_equal(f.last_method, METHOD_PIN_CHAT_MESSAGE);
    assert_non_null(strstr(f.last_body, "\"message_id\":41"));
    assert_non_null(strstr(f.last_body, "\"disable_notification\":true"));
    cmd_call_release(call);
    fake_drain(&f);
}

static void test_unpin_last(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, &wall, "unpin", 0);
    assert_int_equal(cmd_unpin(cmd_call_ctx(call)), 0);
    assert_int_equal(f.last_method, METHOD_UNPIN_CHAT_MESSAGE);
    assert_null(strstr(f.last_body, "message_id"));
    cmd_call_release(call);
    fake_drain(&f);
}

static void test_purge_batches_and_includes_range(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, &wall, "purge", 10);
    /* Recent ids 5..42 in this chat; the command is 42, reply is 10. */
    int64_t ids[64];
    size_t n = 0;
    for (int64_t id = 5; id <= 42; ++id) {
        ids[n++] = id;
    }
    assert_int_equal(cmd_call_set_recent(call, ids, n), 0);
    assert_int_equal(cmd_purge(cmd_call_ctx(call)), 0);
    assert_int_equal(f.calls, 1); /* 33 ids fit in one batch of 100 */
    assert_int_equal(f.last_method, METHOD_DELETE_MESSAGES);
    assert_non_null(strstr(f.last_body, "\"message_ids\":[10,"));
    cmd_call_release(call);
    fake_drain(&f);
}

static void test_purge_splits_into_batches_of_100(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, &wall, "purge", 1);
    cmd_call_message(call)->message_id = 250; /* the command is the last message */
    /* 250 ids in range: Telegram's deleteMessages limit is 100, so /purge must
     * issue three batches (SPEC-ADDENDUM-TOOLCHAIN A5.3 scenario 10). */
    int64_t ids[300];
    size_t n = 0;
    for (int64_t id = 1; id <= 250; ++id) {
        ids[n++] = id;
    }
    assert_int_equal(cmd_call_set_recent(call, ids, n), 0);
    assert_int_equal(cmd_purge(cmd_call_ctx(call)), 0);
    assert_int_equal(f.calls, 3);
    cmd_call_release(call);
    fake_drain(&f);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_del_issues_two_delete_message_calls),
        cmocka_unit_test(test_del_requires_reply),
        cmocka_unit_test(test_pin_silent),
        cmocka_unit_test(test_unpin_last),
        cmocka_unit_test(test_purge_batches_and_includes_range),
        cmocka_unit_test(test_purge_splits_into_batches_of_100),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
