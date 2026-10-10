/* test_cmd_mod.c - moderation handlers over a fake API/DB seam (A5.1).
 *
 * Drives /ban, /mute, /unban and the /kick continuation through a fake
 * tg_api_ops_t that records method + body and lets the test deliver results. The
 * invocation is built with cmd_call_create so the real refcounted lifecycle and
 * reply hook are exercised (no network, no SQLite).
 *
 * Reference model mirrored here: the router holds one reference while the handler
 * runs and releases it afterwards; cmd_ctx_request adds one per in-flight call and
 * its wrapper releases it when the callback returns. The helper `finish()` releases
 * the router's reference and drains every queued completion.
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
#include "tg/db.h"
#include "tg/tg_api.h"
#include "tg/tg_types.h"
#include "tg/util.h"

/* --- Fakes ----------------------------------------------------------------- */

#define FAKE_QUEUE 8u

typedef struct {
    int calls;
    method_id_t method;
    char body[2048];
    struct {
        tg_api_cb cb;
        void *user;
    } pending[FAKE_QUEUE];
    size_t pending_len;
    api_result_t result; /**< Delivered on the next fake_deliver(). */
} fake_api_t;

static int fake_request(void *ctx, method_id_t method, const char *body, size_t body_len,
                        int64_t chat_id, uint64_t deadline_ms, tg_api_cb cb, void *user)
{
    (void)chat_id;
    (void)deadline_ms;
    fake_api_t *f = ctx;
    f->calls++;
    f->method = method;
    size_t n = body_len < sizeof(f->body) - 1u ? body_len : sizeof(f->body) - 1u;
    if (body != NULL) {
        memcpy(f->body, body, n);
    }
    f->body[n] = '\0';
    if (f->pending_len >= FAKE_QUEUE) {
        return -ENOSPC; /* the test never queues this many; guard the bound */
    }
    f->pending[f->pending_len].cb = cb;
    f->pending[f->pending_len].user = user;
    f->pending_len++;
    return 0;
}

/** @brief Deliver the oldest queued completion. */
static void fake_deliver(fake_api_t *f)
{
    if (f->pending_len == 0) {
        return; /* nothing queued; the test never expects a delivery here */
    }
    tg_api_cb cb = f->pending[0].cb;
    void *user = f->pending[0].user;
    for (size_t i = 1; i < f->pending_len; ++i) {
        f->pending[i - 1] = f->pending[i];
    }
    f->pending_len--;
    if (cb != NULL) {
        cb(&f->result, user);
    }
}

/** @brief Deliver every queued completion (in FIFO order) until none remain. */
static void fake_drain(fake_api_t *f)
{
    while (f->pending_len > 0) {
        fake_deliver(f);
    }
}

typedef struct {
    int calls;
    char last_action[32];
    int64_t last_target;
    int64_t last_expires;
} fake_db_t;

static int fake_submit(void *ctx, const db_job_t *job, db_cb cb, void *user)
{
    if (job->kind == DB_JOB_INSERT_ACTION) {
        fake_db_t *d = ctx;
        d->calls++;
        (void)snprintf(d->last_action, sizeof(d->last_action), "%s", job->action);
        d->last_target = job->user_id;
        d->last_expires = job->expires_at;
    }
    if (cb != NULL) {
        db_result_t res = {0};
        cb(&res, user);
    }
    return 0;
}

static int64_t fake_now_unix(void *ctx)
{
    return *(int64_t *)ctx;
}

/* --- Helpers --------------------------------------------------------------- */

/** @brief Build a call + context for a command with a reply-to target. */
static cmd_call_t *make_call(const tg_api_ops_t *api, const db_ops_t *db,
                             const wall_clock_ops_t *wall, const char *cmd_name, int64_t target,
                             const char *args)
{
    cmd_call_t *call = cmd_call_create(api, db, wall);
    assert_non_null(call);
    struct cmd_ctx *ctx = cmd_call_ctx(call);
    tg_message_t *msg = cmd_call_message(call);
    msg->chat.id = -100;
    (void)snprintf(msg->chat.type, sizeof(msg->chat.type), "supergroup");
    msg->message_id = 42;
    msg->from.id = 200;
    msg->has_reply = 1;
    msg->reply_to_message_id = 41;
    msg->reply_from.id = target;
    ctx->cmd = cmd_lookup(cmd_name);
    ctx->chat_id = -100;
    ctx->user_id = 200;
    ctx->bot_id = 999;
    ctx->target_id = target;
    ctx->target_role = ROLE_ANYONE;
    ctx->args = args;
    ctx->is_group = 1;
    ctx->role = ROLE_ADMIN;
    (void)cmd_call_set_target_label(call, "Spammer");
    return call;
}

/**
 * @brief Simulate the router: release its reference, then drain completions.
 *
 * @param[in] call Invocation (the router's reference is dropped here).
 * @param[in] f    Fake API whose queued completions are delivered.
 */
static void finish(cmd_call_t *call, fake_api_t *f)
{
    cmd_call_release(call); /* the router's reference */
    fake_drain(f);
}

/* --- Tests ----------------------------------------------------------------- */

static void test_ban_issues_call_and_records_action(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    fake_db_t d = {0};
    db_ops_t db = {.submit = fake_submit, .ctx = &d};
    int64_t now = 1700000000;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, &db, &wall, "ban", 500, "10m spamming");
    assert_int_equal(cmd_ban(cmd_call_ctx(call)), 0);
    assert_int_equal(f.calls, 1);
    assert_int_equal(f.method, METHOD_BAN_CHAT_MEMBER);
    assert_non_null(strstr(f.body, "\"user_id\":500"));
    assert_non_null(strstr(f.body, "\"until_date\":1700000600"));
    assert_int_equal(d.calls, 1);
    assert_string_equal(d.last_action, "ban");
    assert_int_equal(d.last_target, 500);
    assert_int_equal(d.last_expires, 1700000600);
    finish(call, &f); /* ban completes -> reply */
}

static void test_ban_permanent_has_no_until(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    fake_db_t d = {0};
    db_ops_t db = {.submit = fake_submit, .ctx = &d};
    int64_t now = 1700000000;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, &db, &wall, "ban", 500, "");
    assert_int_equal(cmd_ban(cmd_call_ctx(call)), 0);
    assert_null(strstr(f.body, "until_date"));
    assert_int_equal(d.last_expires, 0);
    finish(call, &f);
}

static void test_mute_sets_all_permissions_false(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    fake_db_t d = {0};
    db_ops_t db = {.submit = fake_submit, .ctx = &d};
    int64_t now = 1700000000;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, &db, &wall, "mute", 500, "30m");
    assert_int_equal(cmd_mute(cmd_call_ctx(call)), 0);
    assert_int_equal(f.method, METHOD_RESTRICT_CHAT_MEMBER);
    assert_non_null(strstr(f.body, "\"can_send_messages\":false"));
    assert_non_null(strstr(f.body, "\"until_date\":1700001800"));
    assert_string_equal(d.last_action, "mute");
    finish(call, &f);
}

static void test_unmute_restores_permissions(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, NULL, &wall, "unmute", 500, "");
    assert_int_equal(cmd_unmute(cmd_call_ctx(call)), 0);
    assert_int_equal(f.method, METHOD_RESTRICT_CHAT_MEMBER);
    assert_non_null(strstr(f.body, "\"can_send_messages\":true"));
    finish(call, &f);
}

static void test_unban_only_if_banned(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, NULL, &wall, "unban", 500, "");
    assert_int_equal(cmd_unban(cmd_call_ctx(call)), 0);
    assert_int_equal(f.method, METHOD_UNBAN_CHAT_MEMBER);
    assert_non_null(strstr(f.body, "\"only_if_banned\":true"));
    finish(call, &f);
}

static void test_kick_chains_ban_then_unban(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, NULL, &wall, "kick", 500, "");
    assert_int_equal(cmd_kick(cmd_call_ctx(call)), 0);
    assert_int_equal(f.calls, 1);
    assert_int_equal(f.method, METHOD_BAN_CHAT_MEMBER);

    cmd_call_release(call); /* router's reference; ban call still holds one */
    f.result = (api_result_t){.status = 0};
    fake_deliver(&f); /* ban success -> issues the unban */
    assert_int_equal(f.calls, 2);
    assert_int_equal(f.method, METHOD_UNBAN_CHAT_MEMBER);
    assert_non_null(strstr(f.body, "\"only_if_banned\":true"));
    fake_drain(&f); /* unban success -> reply */
}

static void test_kick_ban_failure_skips_unban(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, NULL, &wall, "kick", 500, "");
    assert_int_equal(cmd_kick(cmd_call_ctx(call)), 0);
    cmd_call_release(call);
    f.result = (api_result_t){.status = -104};
    (void)snprintf(f.result.description, sizeof(f.result.description), "bad");
    fake_deliver(&f);             /* ban failed -> error reply, no unban */
    assert_int_equal(f.calls, 2); /* ban + error reply */
    assert_int_equal(f.method, METHOD_SEND_MESSAGE);
    fake_drain(&f);
}

static void test_ban_rejects_owner_target(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, NULL, &wall, "ban", 100, "");
    cmd_call_ctx(call)->target_role = ROLE_OWNER;
    assert_int_equal(cmd_ban(cmd_call_ctx(call)), 0);
    assert_int_equal(f.calls, 1); /* only the refusal reply, no ban call */
    assert_int_equal(f.method, METHOD_SEND_MESSAGE);
    finish(call, &f);
}

static void test_ban_no_target_replies(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, NULL, &wall, "ban", 0, "");
    assert_int_equal(cmd_ban(cmd_call_ctx(call)), 0);
    assert_int_equal(f.calls, 1); /* the "no target" reply */
    assert_int_equal(f.method, METHOD_SEND_MESSAGE);
    finish(call, &f);
}

/* --- M5: settings-driven log post and explicit-chat requests ---------------- */

/** @brief Fake API that records the chat_id of every request. */
typedef struct {
    int calls;
    int64_t last_chat;
    method_id_t last_method;
    char last_body[2048];
} chat_api_t;

static int chat_request(void *ctx, method_id_t method, const char *body, size_t body_len,
                        int64_t chat_id, uint64_t deadline_ms, tg_api_cb cb, void *user)
{
    (void)deadline_ms;
    chat_api_t *f = ctx;
    f->calls++;
    f->last_chat = chat_id;
    f->last_method = method;
    size_t n = body_len < sizeof(f->last_body) - 1u ? body_len : sizeof(f->last_body) - 1u;
    if (body != NULL) {
        memcpy(f->last_body, body, n);
    }
    f->last_body[n] = '\0';
    if (cb != NULL) {
        api_result_t res = {0};
        cb(&res, user);
    }
    return 0;
}

static void test_record_action_posts_to_log_channel(void **state)
{
    (void)state;
    chat_api_t f = {0};
    tg_api_ops_t api = {.request = chat_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, NULL, &wall, "ban", 500, "");
    /* Configure a log channel on the invocation's settings. */
    chat_settings_t s = {0};
    s.log_chat_id = -100999;
    assert_int_equal(cmd_ctx_set_settings(cmd_call_ctx(call), &s), 0);

    cmd_call_ctx(call)->record_action(cmd_call_ctx(call), "ban", 500, "spam", 0);
    /* One sendMessage, and it went to the log channel (not the command chat). */
    assert_int_equal(f.calls, 1);
    assert_int_equal(f.last_method, METHOD_SEND_MESSAGE);
    assert_int_equal(f.last_chat, -100999);
    assert_non_null(strstr(f.last_body, "action=ban"));
    assert_non_null(strstr(f.last_body, "Spammer"));
    cmd_call_release(call); /* chat_request delivers synchronously; no drain needed */
}

static void test_record_action_without_log_makes_no_call(void **state)
{
    (void)state;
    chat_api_t f = {0};
    tg_api_ops_t api = {.request = chat_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, NULL, &wall, "ban", 500, "");
    /* No settings: record_action must not post a log message. */
    cmd_call_ctx(call)->record_action(cmd_call_ctx(call), "ban", 500, "spam", 0);
    assert_int_equal(f.calls, 0);
    cmd_call_release(call);
}

static void test_request_chat_uses_explicit_chat(void **state)
{
    (void)state;
    chat_api_t f = {0};
    tg_api_ops_t api = {.request = chat_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, NULL, &wall, "ban", 500, "");
    assert_int_equal(
        cmd_ctx_request_chat(cmd_call_ctx(call), METHOD_SEND_MESSAGE, "{}", 2, -555, NULL, NULL),
        0);
    assert_int_equal(f.last_chat, -555);
    cmd_call_release(call);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_ban_issues_call_and_records_action),
        cmocka_unit_test(test_ban_permanent_has_no_until),
        cmocka_unit_test(test_mute_sets_all_permissions_false),
        cmocka_unit_test(test_unmute_restores_permissions),
        cmocka_unit_test(test_unban_only_if_banned),
        cmocka_unit_test(test_kick_chains_ban_then_unban),
        cmocka_unit_test(test_kick_ban_failure_skips_unban),
        cmocka_unit_test(test_ban_rejects_owner_target),
        cmocka_unit_test(test_ban_no_target_replies),
        cmocka_unit_test(test_record_action_posts_to_log_channel),
        cmocka_unit_test(test_record_action_without_log_makes_no_call),
        cmocka_unit_test(test_request_chat_uses_explicit_chat),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
