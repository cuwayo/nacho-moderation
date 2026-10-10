/* test_db.c - SQLite worker: migration, upserts, job ordering, eventfd (A5.1).
 *
 * Uses a real temporary SQLite file (the worker owns it on its own thread) and the
 * fake ring to check the eventfd arming/re-arm path. Completion callbacks are
 * driven by calling db_pump() in a bounded spin loop.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <cmocka.h>
#include <sqlite3.h>

#include "fake_ring.h"
#include "tg/db.h"
#include "tg/loop.h"

/* --- Helpers --------------------------------------------------------------- */

/* Fill @p buf (at least 32 bytes) with a fresh temp database path; caller unlinks it. */
static int make_db_path(char *buf, size_t cap)
{
    char tmpl[] = "/tmp/nm_db_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    close(fd); /* sqlite will open it by name */
    (void)snprintf(buf, cap, "%s", tmpl);
    return 0;
}

/** @brief Completion capture. */
typedef struct {
    int calls;
    int status;
    int64_t last_id;
} capture_t;

static void on_job(const db_result_t *res, void *ctx)
{
    capture_t *cap = ctx;
    cap->calls++;
    cap->status = res->status;
    cap->last_id = res->i64;
}

/** @brief Completion that copies an expiry-sweep result into a local. */
static void capture_sweep(const db_result_t *res, void *ctx)
{
    db_result_t *out = ctx;
    *out = *res;
    out->status = 1; /* sentinel: the sweep ran */
}

/** @brief Completion that copies a username-lookup result into a local. */
static void capture_lookup(const db_result_t *res, void *ctx)
{
    db_result_t *out = ctx;
    out->i64 = res->i64;
    out->status = 1;
}

/* Spin db_pump until @p cap records @p want calls, or time out. */
static int pump_until(db_t *db, const capture_t *cap, int want)
{
    for (int i = 0; i < 20000; ++i) {
        db_pump(db);
        if (cap->calls >= want) {
            return 0;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000}; /* 1 ms */
        nanosleep(&ts, NULL);
    }
    return -1;
}

/* Query a scalar integer from the DB file (read-only, test-side). */
static long query_scalar(const char *path, const char *sql)
{
    sqlite3 *sdb = NULL;
    long out = -1;
    if (sqlite3_open_v2(path, &sdb, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(sdb);
        return -1;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(sdb, sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        out = (long)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_close(sdb);
    return out;
}

static char *query_text(const char *path, const char *sql)
{
    sqlite3 *sdb = NULL;
    char *out = NULL;
    if (sqlite3_open_v2(path, &sdb, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(sdb);
        return NULL;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(sdb, sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        out = strdup(t != NULL ? (const char *)t : "");
    }
    sqlite3_finalize(st);
    sqlite3_close(sdb);
    return out;
}

/* --- Tests ----------------------------------------------------------------- */

static void test_migration_creates_schema(void **state)
{
    (void)state;
    char path[32];
    assert_int_equal(make_db_path(path, sizeof(path)), 0);

    db_config_t cfg = {.path = path};
    db_t *db = db_create(&cfg);
    assert_non_null(db);

    /* The worker has migrated; read the version and a table count test-side. */
    assert_int_equal(query_scalar(path, "SELECT v FROM schema_version;"), 1);
    assert_int_equal(query_scalar(path, "SELECT count(*) FROM users;"), 0);
    assert_int_equal(query_scalar(path, "SELECT count(*) FROM chats;"), 0);
    assert_int_equal(query_scalar(path, "SELECT count(*) FROM warns;"), 0);
    assert_int_equal(query_scalar(path, "SELECT count(*) FROM actions;"), 0);

    db_destroy(db);
    unlink(path);
}

static void test_migration_is_idempotent(void **state)
{
    (void)state;
    char path[32];
    assert_int_equal(make_db_path(path, sizeof(path)), 0);
    db_config_t cfg = {.path = path};

    db_t *db1 = db_create(&cfg);
    assert_non_null(db1);
    db_destroy(db1);

    /* Reopening the same file must not fail or duplicate the schema. */
    db_t *db2 = db_create(&cfg);
    assert_non_null(db2);
    assert_int_equal(query_scalar(path, "SELECT v FROM schema_version;"), 1);
    db_destroy(db2);

    unlink(path);
}

static void test_upsert_user_and_chat(void **state)
{
    (void)state;
    char path[32];
    assert_int_equal(make_db_path(path, sizeof(path)), 0);
    db_config_t cfg = {.path = path};
    db_t *db = db_create(&cfg);
    assert_non_null(db);

    const db_ops_t *ops = db_ops(db);
    assert_non_null(ops);

    capture_t cap = {0};
    db_job_t ujob = {.kind = DB_JOB_UPSERT_USER, .id = 42, .last_seen = 1700};
    (void)snprintf(ujob.username, sizeof(ujob.username), "ada");
    (void)snprintf(ujob.first_name, sizeof(ujob.first_name), "Ada");
    assert_int_equal(ops->submit(ops->ctx, &ujob, on_job, &cap), 0);

    db_job_t cjob = {.kind = DB_JOB_UPSERT_CHAT, .id = -100123};
    (void)snprintf(cjob.title, sizeof(cjob.title), "Testers");
    (void)snprintf(cjob.chat_type, sizeof(cjob.chat_type), "supergroup");
    assert_int_equal(ops->submit(ops->ctx, &cjob, on_job, &cap), 0);

    assert_int_equal(pump_until(db, &cap, 2), 0);
    assert_int_equal(cap.status, 0);

    assert_int_equal(query_scalar(path, "SELECT count(*) FROM users WHERE user_id=42;"), 1);
    char *name = query_text(path, "SELECT first_name FROM users WHERE user_id=42;");
    assert_non_null(name);
    assert_string_equal(name, "Ada");
    free(name);
    assert_int_equal(query_scalar(path, "SELECT count(*) FROM chats WHERE chat_id=-100123;"), 1);

    db_destroy(db);
    unlink(path);
}

static void test_upsert_replaces_existing(void **state)
{
    (void)state;
    char path[32];
    assert_int_equal(make_db_path(path, sizeof(path)), 0);
    db_config_t cfg = {.path = path};
    db_t *db = db_create(&cfg);
    const db_ops_t *ops = db_ops(db);
    capture_t cap = {0};

    db_job_t j1 = {.kind = DB_JOB_UPSERT_USER, .id = 7, .last_seen = 1};
    (void)snprintf(j1.first_name, sizeof(j1.first_name), "Old");
    (void)ops->submit(ops->ctx, &j1, on_job, &cap);

    db_job_t j2 = {.kind = DB_JOB_UPSERT_USER, .id = 7, .last_seen = 2};
    (void)snprintf(j2.first_name, sizeof(j2.first_name), "New");
    (void)ops->submit(ops->ctx, &j2, on_job, &cap);

    assert_int_equal(pump_until(db, &cap, 2), 0);
    assert_int_equal(query_scalar(path, "SELECT count(*) FROM users WHERE user_id=7;"), 1);
    char *name = query_text(path, "SELECT first_name FROM users WHERE user_id=7;");
    assert_non_null(name);
    assert_string_equal(name, "New");
    free(name);

    db_destroy(db);
    unlink(path);
}

static void test_jobs_processed_in_order(void **state)
{
    (void)state;
    char path[32];
    assert_int_equal(make_db_path(path, sizeof(path)), 0);
    db_config_t cfg = {.path = path};
    db_t *db = db_create(&cfg);
    const db_ops_t *ops = db_ops(db);

    /* Submit several updates to the same user; the final row must be the last job. */
    capture_t cap = {0};
    for (int i = 1; i <= 5; ++i) {
        db_job_t j = {.kind = DB_JOB_UPSERT_USER, .id = 100, .last_seen = i};
        (void)snprintf(j.first_name, sizeof(j.first_name), "name%d", i);
        assert_int_equal(ops->submit(ops->ctx, &j, on_job, &cap), 0);
    }
    assert_int_equal(pump_until(db, &cap, 5), 0);
    assert_int_equal(query_scalar(path, "SELECT last_seen FROM users WHERE user_id=100;"), 5);
    char *name = query_text(path, "SELECT first_name FROM users WHERE user_id=100;");
    assert_non_null(name);
    assert_string_equal(name, "name5");
    free(name);

    db_destroy(db);
    unlink(path);
}

static void test_eventfd_arm_pump_and_rearm(void **state)
{
    (void)state;
    char path[32];
    assert_int_equal(make_db_path(path, sizeof(path)), 0);
    db_config_t cfg = {.path = path};
    db_t *db = db_create(&cfg);

    fake_ring_t ring;
    const loop_ops_t *lops = fake_ring_init(&ring);
    assert_int_equal(db_arm(db, lops), 0);

    /* The read is prepared but not yet submitted by the loop. */
    assert_true(fake_ring_pending_count(&ring) >= 1);
    assert_int_equal(fake_ring_last_type(&ring), OP_EVENTFD);

    capture_t cap = {0};
    db_job_t j = {.kind = DB_JOB_UPSERT_USER, .id = 1};
    (void)snprintf(j.first_name, sizeof(j.first_name), "x");
    const db_ops_t *ops = db_ops(db);
    assert_int_equal(ops->submit(ops->ctx, &j, on_job, &cap), 0);

    /* Drive the loop: submit the armed read, deliver its completion (which pumps
     * and re-arms), until the job's callback has run. */
    int ok = 0;
    for (int i = 0; i < 20000 && !ok; ++i) {
        (void)fake_ring_submit_all(&ring);
        (void)fake_ring_complete_next(&ring, 0);
        if (cap.calls >= 1) {
            ok = 1;
            break;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&ts, NULL);
    }
    assert_int_equal(ok, 1);
    assert_int_equal(cap.calls, 1);
    assert_int_equal(cap.status, 0);
    /* Re-armed: a fresh OP_EVENTFD read is pending after the completion. */
    assert_true(fake_ring_pending_count(&ring) >= 1);

    db_destroy(db);
    unlink(path);
}

static void test_bad_args(void **state)
{
    (void)state;
    assert_null(db_create(NULL));
    db_config_t cfg = {.path = NULL};
    assert_null(db_create(&cfg));
    assert_null(db_ops(NULL));
    assert_int_equal(db_arm(NULL, NULL), -EINVAL);
    db_destroy(NULL); /* no-op */
    db_pump(NULL);    /* no-op */
    assert_int_equal(db_pending(NULL), 0);
}

static void test_action_insert_and_expiry_sweep(void **state)
{
    (void)state;
    char path[32];
    assert_int_equal(make_db_path(path, sizeof(path)), 0);
    db_config_t cfg = {.path = path};
    db_t *db = db_create(&cfg);
    assert_non_null(db);
    const db_ops_t *ops = db_ops(db);
    capture_t cap = {0};

    /* Two actions: one already due (expires_at=100), one in the future. */
    db_job_t due = {.kind = DB_JOB_INSERT_ACTION,
                    .id = -100,
                    .user_id = 7,
                    .admin_id = 1,
                    .created_at = 50,
                    .expires_at = 100};
    (void)snprintf(due.action, sizeof(due.action), "ban");
    assert_int_equal(ops->submit(ops->ctx, &due, on_job, &cap), 0);
    db_job_t future = due;
    future.user_id = 8;
    future.expires_at = 100000;
    assert_int_equal(ops->submit(ops->ctx, &future, on_job, &cap), 0);
    assert_int_equal(pump_until(db, &cap, 2), 0);

    assert_int_equal(query_scalar(path, "SELECT count(*) FROM actions;"), 2);

    /* Sweep at now=200: only the first row is due. */
    db_result_t sweep = {0};
    db_job_t sweep_job = {.kind = DB_JOB_EXPIRE_ACTIONS, .now_unix = 200};
    assert_int_equal(ops->submit(ops->ctx, &sweep_job, capture_sweep, &sweep), 0);
    for (int i = 0; i < 20000 && sweep.status != 1; ++i) {
        db_pump(db);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&ts, NULL);
    }
    assert_int_equal(sweep.status, 1);
    assert_int_equal(sweep.expired_count, 1);
    assert_int_equal(sweep.expired[0].user_id, 7);
    assert_string_equal(sweep.expired[0].action, "ban");

    /* Lift the row; a second sweep finds nothing. */
    db_job_t lift = {.kind = DB_JOB_LIFT_ACTION, .id = sweep.expired[0].id};
    assert_int_equal(ops->submit(ops->ctx, &lift, on_job, &cap), 0);
    assert_int_equal(pump_until(db, &cap, 3), 0);

    db_result_t sweep2 = {0};
    assert_int_equal(ops->submit(ops->ctx, &sweep_job, capture_sweep, &sweep2), 0);
    for (int i = 0; i < 20000 && sweep2.status != 1; ++i) {
        db_pump(db);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&ts, NULL);
    }
    assert_int_equal(sweep2.expired_count, 0);

    db_destroy(db);
    unlink(path);
}

static void test_permanent_action_is_never_swept(void **state)
{
    (void)state;
    char path[32];
    assert_int_equal(make_db_path(path, sizeof(path)), 0);
    db_config_t cfg = {.path = path};
    db_t *db = db_create(&cfg);
    assert_non_null(db);
    const db_ops_t *ops = db_ops(db);
    capture_t cap = {0};

    /* A permanent action stores expires_at as NULL (expires_at=0 in the job). */
    db_job_t perm = {.kind = DB_JOB_INSERT_ACTION,
                     .id = -100,
                     .user_id = 7,
                     .admin_id = 1,
                     .created_at = 50,
                     .expires_at = 0};
    (void)snprintf(perm.action, sizeof(perm.action), "ban");
    assert_int_equal(ops->submit(ops->ctx, &perm, on_job, &cap), 0);
    assert_int_equal(pump_until(db, &cap, 1), 0);

    /* The column is NULL, not 0: the expiry index/query only sees non-NULL. */
    assert_int_equal(query_scalar(path, "SELECT count(*) FROM actions WHERE expires_at IS NULL;"),
                     1);
    assert_int_equal(query_scalar(path, "SELECT count(*) FROM actions WHERE expires_at=0;"), 0);

    /* Sweeping at any time must not return the permanent action (regression: a
     * permanent ban was being unbanned ~30 s after it was issued). */
    db_result_t sweep = {0};
    db_job_t sweep_job = {.kind = DB_JOB_EXPIRE_ACTIONS, .now_unix = 1791627711};
    assert_int_equal(ops->submit(ops->ctx, &sweep_job, capture_sweep, &sweep), 0);
    for (int i = 0; i < 20000 && sweep.status != 1; ++i) {
        db_pump(db);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&ts, NULL);
    }
    assert_int_equal(sweep.expired_count, 0);
    /* And it is still un-lifted. */
    assert_int_equal(query_scalar(path, "SELECT lifted FROM actions WHERE user_id=7;"), 0);

    db_destroy(db);
    unlink(path);
}

static void test_find_user_by_username(void **state)
{
    (void)state;
    char path[32];
    assert_int_equal(make_db_path(path, sizeof(path)), 0);
    db_config_t cfg = {.path = path};
    db_t *db = db_create(&cfg);
    const db_ops_t *ops = db_ops(db);
    capture_t cap = {0};

    db_job_t u = {.kind = DB_JOB_UPSERT_USER, .id = 42};
    (void)snprintf(u.username, sizeof(u.username), "Ada");
    assert_int_equal(ops->submit(ops->ctx, &u, on_job, &cap), 0);
    assert_int_equal(pump_until(db, &cap, 1), 0);

    /* Case-insensitive lookup finds the row. */
    db_result_t res = {0};
    db_job_t find = {.kind = DB_JOB_FIND_USER_BY_USERNAME};
    (void)snprintf(find.username, sizeof(find.username), "ada");
    assert_int_equal(ops->submit(ops->ctx, &find, capture_lookup, &res), 0);
    for (int i = 0; i < 20000 && res.status != 1; ++i) {
        db_pump(db);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&ts, NULL);
    }
    assert_int_equal(res.i64, 42);

    /* An unknown username resolves to 0. */
    db_result_t miss = {0};
    (void)snprintf(find.username, sizeof(find.username), "nobody");
    assert_int_equal(ops->submit(ops->ctx, &find, capture_lookup, &miss), 0);
    for (int i = 0; i < 20000 && miss.status != 1; ++i) {
        db_pump(db);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&ts, NULL);
    }
    assert_int_equal(miss.i64, 0);

    db_destroy(db);
    unlink(path);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_migration_creates_schema),
        cmocka_unit_test(test_migration_is_idempotent),
        cmocka_unit_test(test_upsert_user_and_chat),
        cmocka_unit_test(test_upsert_replaces_existing),
        cmocka_unit_test(test_jobs_processed_in_order),
        cmocka_unit_test(test_eventfd_arm_pump_and_rearm),
        cmocka_unit_test(test_action_insert_and_expiry_sweep),
        cmocka_unit_test(test_permanent_action_is_never_swept),
        cmocka_unit_test(test_find_user_by_username),
        cmocka_unit_test(test_bad_args),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
