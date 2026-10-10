/* test_settings.c - per-chat policy cache over a fake DB seam (A5.1).
 *
 * The service mirrors the administrator cache: a miss submits a DB job and the
 * callback runs when the reply arrives; a hit resolves synchronously; an
 * invalidate forces a refetch. No network, no SQLite, no real clock.
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

#include "tg/db.h"
#include "tg/settings.h"
#include "tg/util.h"

/* --- Fakes ----------------------------------------------------------------- */

typedef struct {
    int submits;
    db_job_kind_t last_kind;
    int64_t last_chat;
    chat_settings_t reply; /**< Returned from the next settings query. */
    db_cb pending_cb;
    void *pending_ctx;
} fake_db_t;

static int fake_submit(void *ctx, const db_job_t *job, db_cb cb, void *user)
{
    fake_db_t *d = ctx;
    d->submits++;
    d->last_kind = job->kind;
    d->last_chat = job->id;
    d->pending_cb = cb;
    d->pending_ctx = user;
    return 0;
}

static void fake_deliver(fake_db_t *d)
{
    db_cb cb = d->pending_cb;
    d->pending_cb = NULL;
    if (cb != NULL) {
        db_result_t res = {.status = 0, .settings = d->reply};
        cb(&res, d->pending_ctx);
    }
}

static uint64_t fake_now_ms(void *ctx)
{
    return *(uint64_t *)ctx;
}

typedef struct {
    int calls;
    int status;
    chat_settings_t settings;
} capture_t;

static void on_settings(const chat_settings_t *s, int status, void *ctx)
{
    capture_t *cap = ctx;
    cap->calls++;
    cap->status = status;
    if (s != NULL) {
        cap->settings = *s;
    }
}

/* --- Tests ----------------------------------------------------------------- */

static void test_miss_then_hit(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    fake_db_t db = {0};
    db_ops_t dbops = {.submit = fake_submit, .ctx = &db};
    settings_config_t cfg = {.db = &dbops, .clock = &clock};
    settings_t *s = settings_create(&cfg);
    assert_non_null(s);

    db.reply.warn_limit = 7;
    (void)snprintf(db.reply.warn_action, sizeof(db.reply.warn_action), "ban");

    /* Miss: a DB query is submitted; the callback is pending. */
    capture_t cap = {0};
    assert_int_equal(settings_get(s, -100, on_settings, &cap), 0);
    assert_int_equal(db.submits, 1);
    assert_int_equal(db.last_kind, DB_JOB_SETTINGS_GET);
    assert_int_equal(db.last_chat, -100);
    assert_int_equal(cap.calls, 0);
    fake_deliver(&db);
    assert_int_equal(cap.calls, 1);
    assert_int_equal(cap.status, 0);
    assert_int_equal(cap.settings.warn_limit, 7);
    assert_string_equal(cap.settings.warn_action, "ban");

    /* Hit: resolved synchronously, no new DB query. */
    capture_t cap2 = {0};
    assert_int_equal(settings_get(s, -100, on_settings, &cap2), 0);
    assert_int_equal(db.submits, 1); /* unchanged */
    assert_int_equal(cap2.calls, 1);
    assert_int_equal(cap2.settings.warn_limit, 7);

    settings_destroy(s);
}

static void test_invalidate_forces_refetch(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    fake_db_t db = {0};
    db_ops_t dbops = {.submit = fake_submit, .ctx = &db};
    settings_config_t cfg = {.db = &dbops, .clock = &clock};
    settings_t *s = settings_create(&cfg);

    db.reply.warn_limit = 1;
    capture_t cap = {0};
    (void)settings_get(s, -100, on_settings, &cap);
    fake_deliver(&db);
    assert_int_equal(db.submits, 1);

    settings_invalidate(s, -100);
    capture_t cap2 = {0};
    (void)settings_get(s, -100, on_settings, &cap2);
    assert_int_equal(db.submits, 2); /* refetched */
    db.reply.warn_limit = 9;
    fake_deliver(&db);
    assert_int_equal(cap2.settings.warn_limit, 9);

    settings_destroy(s);
}

static void test_distinct_chats_are_separate(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    fake_db_t db = {0};
    db_ops_t dbops = {.submit = fake_submit, .ctx = &db};
    settings_config_t cfg = {.db = &dbops, .clock = &clock};
    settings_t *s = settings_create(&cfg);

    db.reply.warn_limit = 3;
    capture_t a = {0};
    (void)settings_get(s, -1, on_settings, &a);
    fake_deliver(&db);
    db.reply.warn_limit = 5;
    capture_t b = {0};
    (void)settings_get(s, -2, on_settings, &b);
    fake_deliver(&db);

    /* Both are cached independently. */
    capture_t a2 = {0};
    capture_t b2 = {0};
    (void)settings_get(s, -1, on_settings, &a2);
    (void)settings_get(s, -2, on_settings, &b2);
    assert_int_equal(a2.settings.warn_limit, 3);
    assert_int_equal(b2.settings.warn_limit, 5);
    assert_int_equal(db.submits, 2);

    settings_destroy(s);
}

static void test_fetch_failure_reports_status(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    fake_db_t db = {0};
    db_ops_t dbops = {.submit = fake_submit, .ctx = &db};
    settings_config_t cfg = {.db = &dbops, .clock = &clock};
    settings_t *s = settings_create(&cfg);

    capture_t cap = {0};
    (void)settings_get(s, -100, on_settings, &cap);
    db_cb cb = db.pending_cb;
    void *pctx = db.pending_ctx;
    db_result_t res = {.status = -106};
    cb(&res, pctx);
    assert_int_equal(cap.calls, 1);
    assert_int_equal(cap.status, -106);

    settings_destroy(s);
}

static void test_bad_args(void **state)
{
    (void)state;
    assert_null(settings_create(NULL));
    settings_config_t cfg = {0};
    assert_null(settings_create(&cfg)); /* no db seam */
    settings_destroy(NULL);             /* no-op */
    settings_invalidate(NULL, 1);       /* no-op */
    assert_int_equal(settings_get(NULL, 1, on_settings, NULL), -EINVAL);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_miss_then_hit),
        cmocka_unit_test(test_invalidate_forces_refetch),
        cmocka_unit_test(test_distinct_chats_are_separate),
        cmocka_unit_test(test_fetch_failure_reports_status),
        cmocka_unit_test(test_bad_args),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
