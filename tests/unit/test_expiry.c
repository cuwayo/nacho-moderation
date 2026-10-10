/* test_expiry.c - the expired-action sweeper (SPEC section 5.6, A5.1).
 *
 * Drives the sweeper through fake DB and API seams: the fake DB returns a scripted
 * list of due actions from the expiry query, and the fake API records the lift
 * calls. Checks that a due `ban` becomes an `unbanChatMember`, a due `mute`
 * becomes a `restrictChatMember` with permissions restored, and that every row is
 * marked lifted. No network, no SQLite, no real clock.
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
#include "fake_ring.h"
#include "tg/api_pool.h"
#include "tg/db.h"
#include "tg/expiry.h"
#include "tg/loop.h"
#include "tg/tg_api.h"
#include "tg/util.h"

/* --- Fakes ----------------------------------------------------------------- */

typedef struct {
    int expire_queries;
    int lift_updates;
    db_result_t scripted; /**< Returned from the next expiry query. */
} fake_db_t;

static int fake_submit(void *ctx, const db_job_t *job, db_cb cb, void *user)
{
    fake_db_t *d = ctx;
    db_result_t res = {0};
    if (job->kind == DB_JOB_EXPIRE_ACTIONS) {
        d->expire_queries++;
        res = d->scripted;
    } else if (job->kind == DB_JOB_LIFT_ACTION) {
        d->lift_updates++;
    }
    if (cb != NULL) {
        cb(&res, user);
    }
    return 0;
}

typedef struct {
    int calls;
    method_id_t method;
    char body[1024];
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
    if (cb != NULL) {
        api_result_t res = {0};
        cb(&res, user);
    }
    return 0;
}

static int64_t fake_now_unix(void *ctx)
{
    return *(int64_t *)ctx;
}

/* --- Tests ----------------------------------------------------------------- */

static void test_sweep_lifts_due_ban_and_mute(void **state)
{
    (void)state;
    fake_db_t db = {0};
    db.scripted.status = 0;
    db.scripted.expired_count = 2;
    db.scripted.expired[0].id = 1;
    db.scripted.expired[0].chat_id = -100;
    db.scripted.expired[0].user_id = 7;
    (void)snprintf(db.scripted.expired[0].action, sizeof(db.scripted.expired[0].action), "ban");
    db.scripted.expired[1].id = 2;
    db.scripted.expired[1].chat_id = -100;
    db.scripted.expired[1].user_id = 8;
    (void)snprintf(db.scripted.expired[1].action, sizeof(db.scripted.expired[1].action), "mute");
    db_ops_t dbops = {.submit = fake_submit, .ctx = &db};

    fake_api_t api = {0};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = &api};

    int64_t now = 1700000000;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    fake_ring_t ring;
    const loop_ops_t *lops = fake_ring_init(&ring);
    expiry_config_t cfg = {.ops = lops, .db = &dbops, .api = &apiops, .wall = &wall};
    expiry_t *e = expiry_create(&cfg);
    assert_non_null(e);

    assert_int_equal(expiry_sweep_now(e), 0);
    assert_int_equal(db.expire_queries, 1);
    assert_int_equal(api.calls, 2);
    /* The mute (second lift) restores permissions. */
    assert_int_equal(api.method, METHOD_RESTRICT_CHAT_MEMBER);
    assert_non_null(strstr(api.body, "\"can_send_messages\":true"));
    /* Both rows were marked lifted. */
    assert_int_equal(db.lift_updates, 2);

    expiry_destroy(e);
}

static void test_sweep_ban_only(void **state)
{
    (void)state;
    fake_db_t db = {0};
    db.scripted.expired_count = 1;
    db.scripted.expired[0].id = 5;
    db.scripted.expired[0].chat_id = -100;
    db.scripted.expired[0].user_id = 7;
    (void)snprintf(db.scripted.expired[0].action, sizeof(db.scripted.expired[0].action), "ban");
    db_ops_t dbops = {.submit = fake_submit, .ctx = &db};

    fake_api_t api = {0};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = &api};

    int64_t now = 1700000000;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    fake_ring_t ring;
    const loop_ops_t *lops = fake_ring_init(&ring);
    expiry_config_t cfg = {.ops = lops, .db = &dbops, .api = &apiops, .wall = &wall};
    expiry_t *e = expiry_create(&cfg);
    assert_non_null(e);
    assert_int_equal(expiry_sweep_now(e), 0);
    assert_int_equal(api.calls, 1);
    assert_int_equal(api.method, METHOD_UNBAN_CHAT_MEMBER);
    assert_non_null(strstr(api.body, "\"only_if_banned\":true"));
    assert_int_equal(db.lift_updates, 1);
    expiry_destroy(e);
}

static void test_no_expired_actions(void **state)
{
    (void)state;
    fake_db_t db = {0};
    db.scripted.expired_count = 0;
    db_ops_t dbops = {.submit = fake_submit, .ctx = &db};
    fake_api_t api = {0};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = &api};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    fake_ring_t ring;
    const loop_ops_t *lops = fake_ring_init(&ring);
    expiry_config_t cfg = {.ops = lops, .db = &dbops, .api = &apiops, .wall = &wall};
    expiry_t *e = expiry_create(&cfg);
    assert_int_equal(expiry_sweep_now(e), 0);
    assert_int_equal(api.calls, 0);
    assert_int_equal(db.lift_updates, 0);
    expiry_destroy(e);
}

static void test_bad_args(void **state)
{
    (void)state;
    assert_null(expiry_create(NULL));
    assert_int_equal(expiry_sweep_now(NULL), -EINVAL);
    expiry_stop(NULL);    /* no-op */
    expiry_destroy(NULL); /* no-op */
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_sweep_lifts_due_ban_and_mute),
        cmocka_unit_test(test_sweep_ban_only),
        cmocka_unit_test(test_no_expired_actions),
        cmocka_unit_test(test_bad_args),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
