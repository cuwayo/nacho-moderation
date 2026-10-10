/* test_cmd_warn.c - warning handlers over fake API/DB seams (A5.1).
 *
 * Drives /warn, /unwarn, /warns and /resetwarns through a fake tg_api_ops_t and a
 * fake db_ops_t. The DB fake answers warn jobs synchronously so the async chain
 * completes inside the handler call. No network, no SQLite.
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

typedef struct {
    int calls;
    method_id_t last_method;
    int saw_ban;
    int saw_mute;
    char last_body[2048];
} fake_api_t;

static int fake_request(void *ctx, method_id_t method, const char *body, size_t body_len,
                        int64_t chat_id, uint64_t deadline_ms, tg_api_cb cb, void *user)
{
    (void)chat_id;
    (void)deadline_ms;
    fake_api_t *f = ctx;
    f->calls++;
    f->last_method = method;
    if (method == METHOD_BAN_CHAT_MEMBER) {
        f->saw_ban = 1;
    }
    if (method == METHOD_RESTRICT_CHAT_MEMBER) {
        f->saw_mute = 1;
    }
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

typedef struct {
    int add_calls;
    int remove_calls;
    int reset_calls;
    int list_calls;
    int64_t last_list_user; /**< user_id of the most recent warn-list query. */
    int64_t next_count;     /**< Returned as the warn count for an add/list. */
    int add_status;
} fake_db_t;

static int fake_submit(void *ctx, const db_job_t *job, db_cb cb, void *user)
{
    fake_db_t *d = ctx;
    db_result_t res = {0};
    switch (job->kind) {
    case DB_JOB_WARN_ADD:
        d->add_calls++;
        res.status = d->add_status;
        res.i64 = d->next_count;
        break;
    case DB_JOB_WARN_REMOVE_LAST:
        d->remove_calls++;
        break;
    case DB_JOB_WARN_RESET:
        d->reset_calls++;
        break;
    case DB_JOB_WARN_LIST:
        d->list_calls++;
        d->last_list_user = job->user_id;
        res.warn_count = 1;
        (void)snprintf(res.warns[0].reason, sizeof(res.warns[0].reason), "spam");
        break;
    default:
        break;
    }
    if (cb != NULL) {
        cb(&res, user);
    }
    return 0;
}

static int64_t fake_now_unix(void *ctx)
{
    return *(int64_t *)ctx;
}

/* --- Helpers --------------------------------------------------------------- */

static cmd_call_t *make_call(const tg_api_ops_t *api, const db_ops_t *db,
                             const wall_clock_ops_t *wall, const char *name, int64_t target,
                             const char *args, const chat_settings_t *settings)
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
    ctx->cmd = cmd_lookup(name);
    ctx->chat_id = -100;
    ctx->user_id = 200;
    ctx->bot_id = 999;
    ctx->target_id = target;
    ctx->target_role = ROLE_ANYONE;
    ctx->args = args;
    ctx->is_group = 1;
    ctx->role = ROLE_ADMIN;
    (void)cmd_call_set_target_label(call, "Spammer");
    if (settings != NULL) {
        (void)cmd_ctx_set_settings(ctx, settings);
    }
    return call;
}

/* --- Tests ----------------------------------------------------------------- */

static void test_warn_below_limit_reports_count(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    fake_db_t d = {0};
    d.next_count = 1;
    db_ops_t db = {.submit = fake_submit, .ctx = &d};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};
    chat_settings_t s = {0};
    s.warn_limit = 3;
    (void)snprintf(s.warn_action, sizeof(s.warn_action), "mute");

    cmd_call_t *call = make_call(&api, &db, &wall, "warn", 500, "spam", &s);
    assert_int_equal(cmd_warn(cmd_call_ctx(call)), 0);
    assert_int_equal(d.add_calls, 1);
    assert_int_equal(f.calls, 1); /* the "warned (1/3)" reply */
    assert_int_equal(f.last_method, METHOD_SEND_MESSAGE);
    assert_null(strstr(f.last_body, "reached"));
    cmd_call_release(call);
}

static void test_warn_at_limit_applies_action_and_resets(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    fake_db_t d = {0};
    d.next_count = 3;
    db_ops_t db = {.submit = fake_submit, .ctx = &d};
    int64_t now = 1700000000;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};
    chat_settings_t s = {0};
    s.warn_limit = 3;
    (void)snprintf(s.warn_action, sizeof(s.warn_action), "ban");

    cmd_call_t *call = make_call(&api, &db, &wall, "warn", 500, "spam", &s);
    assert_int_equal(cmd_warn(cmd_call_ctx(call)), 0);
    assert_int_equal(d.add_calls, 1);
    /* A ban call was issued for the target, and the warnings were reset. */
    assert_int_equal(f.saw_ban, 1);
    assert_int_equal(d.reset_calls, 1);
    cmd_call_release(call);
}

static void test_warn_no_target_replies(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    fake_db_t d = {0};
    db_ops_t db = {.submit = fake_submit, .ctx = &d};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, &db, &wall, "warn", 0, "", NULL);
    assert_int_equal(cmd_warn(cmd_call_ctx(call)), 0);
    assert_int_equal(d.add_calls, 0);
    assert_int_equal(f.calls, 1); /* the "no target" reply */
    cmd_call_release(call);
}

static void test_unwarn_and_reset_issue_jobs(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    fake_db_t d = {0};
    db_ops_t db = {.submit = fake_submit, .ctx = &d};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *c1 = make_call(&api, &db, &wall, "unwarn", 500, "", NULL);
    assert_int_equal(cmd_unwarn(cmd_call_ctx(c1)), 0);
    assert_int_equal(d.remove_calls, 1);
    cmd_call_release(c1);

    cmd_call_t *c2 = make_call(&api, &db, &wall, "resetwarns", 500, "", NULL);
    assert_int_equal(cmd_resetwarns(cmd_call_ctx(c2)), 0);
    assert_int_equal(d.reset_calls, 1);
    cmd_call_release(c2);
}

static void test_warns_lists_other_for_admin(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    fake_db_t d = {0};
    db_ops_t db = {.submit = fake_submit, .ctx = &d};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    /* An admin names another user (target 500 != caller 200). */
    cmd_call_t *call = make_call(&api, &db, &wall, "warns", 500, "", NULL);
    assert_int_equal(cmd_warns(cmd_call_ctx(call)), 0);
    assert_int_equal(d.list_calls, 1);
    assert_int_equal(d.last_list_user, 500);
    assert_int_equal(f.last_method, METHOD_SEND_MESSAGE);
    assert_non_null(strstr(f.last_body, "spam"));
    cmd_call_release(call);
}

static void test_warns_self_for_member(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    fake_db_t d = {0};
    db_ops_t db = {.submit = fake_submit, .ctx = &d};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    /* A plain member with no target lists their own warnings (self-service). */
    cmd_call_t *call = make_call(&api, &db, &wall, "warns", 0, "", NULL);
    cmd_call_ctx(call)->role = ROLE_ANYONE;
    assert_int_equal(cmd_warns(cmd_call_ctx(call)), 0);
    assert_int_equal(d.list_calls, 1);
    assert_int_equal(d.last_list_user, 200); /* the caller's own id */
    assert_non_null(strstr(f.last_body, "You have"));
    cmd_call_release(call);
}

static void test_warns_self_for_member_with_reply_to_self(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    fake_db_t d = {0};
    db_ops_t db = {.submit = fake_submit, .ctx = &d};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    /* A member replying to their own message is still a self-listing. */
    cmd_call_t *call = make_call(&api, &db, &wall, "warns", 200, "", NULL);
    cmd_call_ctx(call)->role = ROLE_ANYONE;
    assert_int_equal(cmd_warns(cmd_call_ctx(call)), 0);
    assert_int_equal(d.list_calls, 1);
    assert_int_equal(d.last_list_user, 200);
    cmd_call_release(call);
}

static void test_warns_other_denied_for_member(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    fake_db_t d = {0};
    db_ops_t db = {.submit = fake_submit, .ctx = &d};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    /* A member naming another user is refused without a DB query. */
    cmd_call_t *call = make_call(&api, &db, &wall, "warns", 500, "", NULL);
    cmd_call_ctx(call)->role = ROLE_ANYONE;
    assert_int_equal(cmd_warns(cmd_call_ctx(call)), 0);
    assert_int_equal(d.list_calls, 0);
    assert_int_equal(f.last_method, METHOD_SEND_MESSAGE);
    assert_non_null(strstr(f.last_body, "administrators"));
    cmd_call_release(call);
}

static void test_warn_add_failure_reports(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    fake_db_t d = {0};
    d.add_status = -106;
    db_ops_t db = {.submit = fake_submit, .ctx = &d};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, &db, &wall, "warn", 500, "spam", NULL);
    assert_int_equal(cmd_warn(cmd_call_ctx(call)), 0);
    assert_int_equal(f.calls, 1);
    assert_non_null(strstr(f.last_body, "could not record"));
    cmd_call_release(call);
}

static void test_warn_limit_mute_action(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    fake_db_t d = {0};
    d.next_count = 2;
    db_ops_t db = {.submit = fake_submit, .ctx = &d};
    int64_t now = 1700000000;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};
    chat_settings_t s = {0};
    s.warn_limit = 2;
    (void)snprintf(s.warn_action, sizeof(s.warn_action), "mute");

    cmd_call_t *call = make_call(&api, &db, &wall, "warn", 500, "spam", &s);
    assert_int_equal(cmd_warn(cmd_call_ctx(call)), 0);
    assert_int_equal(f.saw_mute, 1);
    assert_int_equal(d.reset_calls, 1);
    cmd_call_release(call);
}

static void test_warn_limit_kick_action(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    fake_db_t d = {0};
    d.next_count = 1;
    db_ops_t db = {.submit = fake_submit, .ctx = &d};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};
    chat_settings_t s = {0};
    s.warn_limit = 1;
    (void)snprintf(s.warn_action, sizeof(s.warn_action), "kick");

    cmd_call_t *call = make_call(&api, &db, &wall, "warn", 500, "spam", &s);
    assert_int_equal(cmd_warn(cmd_call_ctx(call)), 0);
    /* The kick chains a ban then an unban; the reset happens after the ban. */
    assert_int_equal(f.saw_ban, 1);
    assert_int_equal(d.reset_calls, 1);
    cmd_call_release(call);
}

static void test_warn_limit_zero_disables_action(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    fake_db_t d = {0};
    d.next_count = 99;
    db_ops_t db = {.submit = fake_submit, .ctx = &d};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};
    chat_settings_t s = {0};
    s.warn_limit = 0; /* auto-action disabled */

    cmd_call_t *call = make_call(&api, &db, &wall, "warn", 500, "spam", &s);
    assert_int_equal(cmd_warn(cmd_call_ctx(call)), 0);
    assert_int_equal(f.saw_ban, 0);
    assert_int_equal(f.saw_mute, 0);
    assert_int_equal(d.reset_calls, 0);
    cmd_call_release(call);
}

static void test_warn_without_db_replies_unavailable(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, NULL, &wall, "warn", 500, "spam", NULL);
    assert_int_equal(cmd_warn(cmd_call_ctx(call)), 0);
    assert_int_equal(f.calls, 1);
    assert_non_null(strstr(f.last_body, "unavailable"));
    cmd_call_release(call);
}

static void test_unwarn_no_target_replies(void **state)
{
    (void)state;
    fake_api_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    fake_db_t d = {0};
    db_ops_t db = {.submit = fake_submit, .ctx = &d};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *call = make_call(&api, &db, &wall, "unwarn", 0, "", NULL);
    assert_int_equal(cmd_unwarn(cmd_call_ctx(call)), 0);
    assert_int_equal(d.remove_calls, 0);
    assert_int_equal(f.calls, 1);
    cmd_call_release(call);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_warn_below_limit_reports_count),
        cmocka_unit_test(test_warn_at_limit_applies_action_and_resets),
        cmocka_unit_test(test_warn_no_target_replies),
        cmocka_unit_test(test_unwarn_and_reset_issue_jobs),
        cmocka_unit_test(test_warns_lists_other_for_admin),
        cmocka_unit_test(test_warns_self_for_member),
        cmocka_unit_test(test_warns_self_for_member_with_reply_to_self),
        cmocka_unit_test(test_warns_other_denied_for_member),
        cmocka_unit_test(test_warn_add_failure_reports),
        cmocka_unit_test(test_warn_limit_mute_action),
        cmocka_unit_test(test_warn_limit_kick_action),
        cmocka_unit_test(test_warn_limit_zero_disables_action),
        cmocka_unit_test(test_warn_without_db_replies_unavailable),
        cmocka_unit_test(test_unwarn_no_target_replies),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
