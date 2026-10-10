/**
 * @file db.c
 * @brief SQLite worker thread, job queue, migrations, and eventfd handoff.
 *
 * See db.h for the design and ADR 0003. The main thread only ever touches the
 * job/response queues (under a mutex) and the eventfd; SQLite and its prepared
 * statements are owned exclusively by the worker thread.
 *
 * @ingroup db
 */
#include "tg/db.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <unistd.h>

#include <sqlite3.h>

#include "tg/errors.h"
#include "tg/loop.h"

/* Reusable upsert statements (SQL is never string-concatenated, ADR 0003). */
/** @brief Prepared upsert for the `users` table. */
#define DB_SQL_USERS                                                                               \
    "INSERT INTO users(user_id, username, first_name, last_seen) VALUES(?1, ?2, ?3, ?4) "          \
    "ON CONFLICT(user_id) DO UPDATE SET username=excluded.username, "                              \
    "first_name=excluded.first_name, last_seen=excluded.last_seen"
/** @brief Prepared upsert for the `chats` table. */
#define DB_SQL_CHATS                                                                               \
    "INSERT INTO chats(chat_id, title, type) VALUES(?1, ?2, ?3) "                                  \
    "ON CONFLICT(chat_id) DO UPDATE SET title=excluded.title, type=excluded.type"
/** @brief Prepared insert for an expiring moderation action. */
#define DB_SQL_ACTION                                                                              \
    "INSERT INTO actions(chat_id, user_id, admin_id, action, reason, created_at, expires_at) "     \
    "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)"
/** @brief Select due, un-lifted expiring actions (SPEC section 5.6). */
#define DB_SQL_EXPIRE                                                                              \
    "SELECT id, chat_id, user_id, action FROM actions "                                            \
    "WHERE lifted=0 AND expires_at IS NOT NULL AND expires_at<=?1 LIMIT ?2"
/** @brief Mark one action row as lifted. */
#define DB_SQL_LIFT "UPDATE actions SET lifted=1 WHERE id=?1"
/** @brief Resolve a username (case-insensitive) to a user id. */
#define DB_SQL_FIND_USER "SELECT user_id FROM users WHERE username=?1 COLLATE NOCASE LIMIT 1"

/** @brief Schema migration v0 -> v1 (SPEC section 6). Idempotent via IF NOT EXISTS. */
static const char *const SCHEMA_V1 =
    "CREATE TABLE IF NOT EXISTS chats ("
    " chat_id INTEGER PRIMARY KEY, title TEXT, type TEXT,"
    " welcome_enabled INTEGER DEFAULT 0, welcome_text TEXT, rules_text TEXT,"
    " captcha_enabled INTEGER DEFAULT 0, captcha_timeout INTEGER DEFAULT 120,"
    " warn_limit INTEGER DEFAULT 3, warn_action TEXT DEFAULT 'mute',"
    " flood_limit INTEGER DEFAULT 0, flood_window INTEGER DEFAULT 10,"
    " locks INTEGER DEFAULT 0, log_chat_id INTEGER, lang TEXT DEFAULT 'en');"
    "CREATE TABLE IF NOT EXISTS users ("
    " user_id INTEGER PRIMARY KEY, username TEXT COLLATE NOCASE,"
    " first_name TEXT, last_seen INTEGER);"
    "CREATE INDEX IF NOT EXISTS idx_users_username ON users(username);"
    "CREATE TABLE IF NOT EXISTS warns ("
    " id INTEGER PRIMARY KEY, chat_id INTEGER, user_id INTEGER,"
    " admin_id INTEGER, reason TEXT, created_at INTEGER);"
    "CREATE INDEX IF NOT EXISTS idx_warns ON warns(chat_id, user_id);"
    "CREATE TABLE IF NOT EXISTS actions ("
    " id INTEGER PRIMARY KEY, chat_id INTEGER, user_id INTEGER, admin_id INTEGER,"
    " action TEXT, reason TEXT, created_at INTEGER, expires_at INTEGER,"
    " lifted INTEGER DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS idx_actions_exp ON actions(expires_at)"
    " WHERE lifted=0 AND expires_at IS NOT NULL;"
    "CREATE TABLE IF NOT EXISTS blacklist ("
    " chat_id INTEGER, pattern TEXT, is_regex INTEGER DEFAULT 0,"
    " PRIMARY KEY (chat_id, pattern));"
    "CREATE TABLE IF NOT EXISTS pending_captcha ("
    " chat_id INTEGER, user_id INTEGER, message_id INTEGER,"
    " nonce TEXT, deadline INTEGER, PRIMARY KEY (chat_id, user_id));";

/** @brief A queued job plus its completion callback. */
typedef struct db_req {
    struct db_req *next; /**< FIFO link. */
    db_job_t job;        /**< Job value (copied). */
    db_cb cb;            /**< Completion callback. */
    void *user;          /**< Callback context. */
} db_req_t;

/** @brief A completed job awaiting dispatch on the loop thread. */
typedef struct db_resp {
    struct db_resp *next; /**< FIFO link. */
    db_result_t res;      /**< Outcome. */
    db_cb cb;             /**< Completion callback. */
    void *user;           /**< Callback context. */
} db_resp_t;

/** @brief The DB worker: queues, synchronization, eventfd, and the worker handle. */
struct db {
    char path[4096]; /**< Copied database path. */

    pthread_t thread;        /**< Worker thread handle. */
    pthread_mutex_t mu;      /**< Guards every queue and the flags below. */
    pthread_cond_t job_cv;   /**< Signals the worker that jobs are queued. */
    pthread_cond_t ready_cv; /**< Signals the creator that startup finished. */
    int stop;                /**< 1 once shutdown was requested. */
    int ready;               /**< 1 once the worker finished startup. */
    int init_status;         /**< Startup result (0 on success). */

    db_req_t *job_head;   /**< Job queue head. */
    db_req_t *job_tail;   /**< Job queue tail. */
    db_resp_t *resp_head; /**< Response queue head. */
    db_resp_t *resp_tail; /**< Response queue tail. */
    size_t pending;       /**< Jobs submitted but not yet dispatched (loop-side count). */

    int efd;               /**< eventfd written by the worker. */
    op_t event_op;         /**< OP_EVENTFD op. */
    const loop_ops_t *ops; /**< Loop vtable (set by db_arm). */
    int event_armed;       /**< 1 while an eventfd read is outstanding. */
    db_ops_t seam;         /**< Injectable seam bound to this worker. */
};

/* --- Worker-side SQLite ---------------------------------------------------- */

/**
 * @brief Apply the pragmas required by SPEC section 4.4.
 *
 * @param[in] sdb Open SQLite handle.
 *
 * @return 0 on success, -ERR_DB on failure.
 */
static int apply_pragmas(sqlite3 *sdb)
{
    const char *pragmas[] = {"PRAGMA journal_mode=WAL;", "PRAGMA synchronous=NORMAL;",
                             "PRAGMA foreign_keys=ON;", "PRAGMA busy_timeout=5000;"};
    for (size_t i = 0; i < sizeof(pragmas) / sizeof(pragmas[0]); ++i) {
        if (sqlite3_exec(sdb, pragmas[i], NULL, NULL, NULL) != SQLITE_OK) {
            return -ERR_DB;
        }
    }
    return 0;
}

/**
 * @brief Read the current schema version, creating the table when absent.
 *
 * @param[in]  sdb Open SQLite handle.
 * @param[out] out Receives the version (0 when the table is empty/absent).
 *
 * @return 0 on success, -ERR_DB on failure.
 */
static int read_schema_version(sqlite3 *sdb, int *out)
{
    *out = 0;
    if (sqlite3_exec(sdb, "CREATE TABLE IF NOT EXISTS schema_version (v INTEGER);", NULL, NULL,
                     NULL) != SQLITE_OK) {
        return -ERR_DB;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(sdb, "SELECT v FROM schema_version LIMIT 1;", -1, &st, NULL) !=
        SQLITE_OK) {
        return -ERR_DB;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        *out = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return 0;
}

/**
 * @brief Run migrations from the current version to the latest.
 *
 * @param[in] sdb Open SQLite handle.
 *
 * @return 0 on success, -ERR_DB on failure.
 */
static int migrate(sqlite3 *sdb)
{
    int v = 0;
    int rc = read_schema_version(sdb, &v);
    if (rc != 0) {
        return rc;
    }
    if (v >= 1) {
        return 0;
    }
    if (sqlite3_exec(sdb, SCHEMA_V1, NULL, NULL, NULL) != SQLITE_OK) {
        return -ERR_DB;
    }
    if (sqlite3_exec(sdb, "DELETE FROM schema_version; INSERT INTO schema_version(v) VALUES(1);",
                     NULL, NULL, NULL) != SQLITE_OK) {
        return -ERR_DB;
    }
    return 0;
}

/** @brief Reusable prepared statements owned by the worker thread. */
typedef struct {
    sqlite3_stmt *upsert_user;    /**< DB_SQL_USERS. */
    sqlite3_stmt *upsert_chat;    /**< DB_SQL_CHATS. */
    sqlite3_stmt *insert_action;  /**< DB_SQL_ACTION. */
    sqlite3_stmt *expire_actions; /**< DB_SQL_EXPIRE. */
    sqlite3_stmt *lift_action;    /**< DB_SQL_LIFT. */
    sqlite3_stmt *find_user;      /**< DB_SQL_FIND_USER. */
} db_stmts_t;

/**
 * @brief Open the database and prepare the reusable statements.
 *
 * @param[in]  path Path to the SQLite file.
 * @param[out] out_db Receives the handle.
 * @param[out] out_st Receives the prepared statements.
 *
 * @return 0 on success, -ERR_DB on failure (partial handles are cleaned up).
 */
static int worker_open(const char *path, sqlite3 **out_db, db_stmts_t *out_st)
{
    sqlite3 *sdb = NULL;
    if (sqlite3_open_v2(path, &sdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) !=
        SQLITE_OK) {
        sqlite3_close(sdb);
        return -ERR_DB;
    }
    if (apply_pragmas(sdb) != 0 || migrate(sdb) != 0) {
        sqlite3_close(sdb);
        return -ERR_DB;
    }
    db_stmts_t st = {0};
    struct {
        sqlite3_stmt **slot;
        const char *sql;
    } prep[] = {
        {&st.upsert_user, DB_SQL_USERS},    {&st.upsert_chat, DB_SQL_CHATS},
        {&st.insert_action, DB_SQL_ACTION}, {&st.expire_actions, DB_SQL_EXPIRE},
        {&st.lift_action, DB_SQL_LIFT},     {&st.find_user, DB_SQL_FIND_USER},
    };
    for (size_t i = 0; i < sizeof(prep) / sizeof(prep[0]); ++i) {
        if (sqlite3_prepare_v2(sdb, prep[i].sql, -1, prep[i].slot, NULL) != SQLITE_OK) {
            sqlite3_finalize(st.upsert_user);
            sqlite3_finalize(st.upsert_chat);
            sqlite3_finalize(st.insert_action);
            sqlite3_finalize(st.expire_actions);
            sqlite3_finalize(st.lift_action);
            sqlite3_finalize(st.find_user);
            sqlite3_close(sdb);
            return -ERR_DB;
        }
    }
    *out_db = sdb;
    *out_st = st;
    return 0;
}

/**
 * @brief Run a prepared statement expected to complete with SQLITE_DONE.
 *
 * @param[in] s Statement to step.
 *
 * @return 0 on success, -ERR_DB on failure.
 */
static int step_done(sqlite3_stmt *s)
{
    int rc = sqlite3_step(s) == SQLITE_DONE ? 0 : -ERR_DB;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    return rc;
}

/**
 * @brief Execute one job with the prepared statements.
 *
 * @param[in] st  Prepared statements.
 * @param[in] job Job to run.
 * @param[out] res Result to fill (only the fields the job produces).
 *
 * @return 0 on success, -ERR_DB on failure.
 */
static int worker_execute(db_stmts_t *st, const db_job_t *job, db_result_t *res)
{
    switch (job->kind) {
    case DB_JOB_UPSERT_USER: {
        sqlite3_stmt *s = st->upsert_user;
        sqlite3_bind_int64(s, 1, job->id);
        sqlite3_bind_text(s, 2, job->username, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, job->first_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 4, job->last_seen);
        return step_done(s);
    }
    case DB_JOB_UPSERT_CHAT: {
        sqlite3_stmt *s = st->upsert_chat;
        sqlite3_bind_int64(s, 1, job->id);
        sqlite3_bind_text(s, 2, job->title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, job->chat_type, -1, SQLITE_TRANSIENT);
        return step_done(s);
    }
    case DB_JOB_INSERT_ACTION: {
        sqlite3_stmt *s = st->insert_action;
        sqlite3_bind_int64(s, 1, job->id); /* chat_id */
        sqlite3_bind_int64(s, 2, job->user_id);
        sqlite3_bind_int64(s, 3, job->admin_id);
        sqlite3_bind_text(s, 4, job->action, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 5, job->reason, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 6, job->created_at);
        sqlite3_bind_int64(s, 7, job->expires_at);
        return step_done(s);
    }
    case DB_JOB_LIFT_ACTION: {
        sqlite3_stmt *s = st->lift_action;
        sqlite3_bind_int64(s, 1, job->id);
        return step_done(s);
    }
    case DB_JOB_FIND_USER_BY_USERNAME: {
        sqlite3_stmt *s = st->find_user;
        sqlite3_bind_text(s, 1, job->username, -1, SQLITE_TRANSIENT);
        res->i64 = 0;
        if (sqlite3_step(s) == SQLITE_ROW) {
            res->i64 = sqlite3_column_int64(s, 0);
        }
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        return 0;
    }
    case DB_JOB_EXPIRE_ACTIONS: {
        sqlite3_stmt *s = st->expire_actions;
        sqlite3_bind_int64(s, 1, job->now_unix);
        sqlite3_bind_int(s, 2, (int)DB_MAX_EXPIRED);
        res->expired_count = 0;
        while (res->expired_count < DB_MAX_EXPIRED && sqlite3_step(s) == SQLITE_ROW) {
            db_expired_action_t *e = &res->expired[res->expired_count];
            e->id = sqlite3_column_int64(s, 0);
            e->chat_id = sqlite3_column_int64(s, 1);
            e->user_id = sqlite3_column_int64(s, 2);
            const unsigned char *act = sqlite3_column_text(s, 3);
            (void)snprintf(e->action, sizeof(e->action), "%s",
                           act != NULL ? (const char *)act : "");
            res->expired_count++;
        }
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        return 0;
    }
    default:
        return -ERR_DB;
    }
}

/* --- Worker thread --------------------------------------------------------- */

/**
 * @brief Worker main: open the database, then process jobs until shutdown.
 *
 * Signals readiness (success or failure) through @ref db.ready_cv before entering
 * the job loop, so @ref db_create can report an open/migrate error synchronously.
 *
 * @param[in] arg The @ref db_t.
 *
 * @return NULL.
 */
static void *db_worker(void *arg)
{
    db_t *db = arg;

    sqlite3 *sdb = NULL;
    db_stmts_t st = {0};
    int rc = worker_open(db->path, &sdb, &st);

    pthread_mutex_lock(&db->mu);
    db->init_status = rc;
    db->ready = 1;
    pthread_cond_broadcast(&db->ready_cv);
    pthread_mutex_unlock(&db->mu);

    if (rc != 0) {
        return NULL;
    }

    for (;;) {
        pthread_mutex_lock(&db->mu);
        while (db->job_head == NULL && !db->stop) {
            pthread_cond_wait(&db->job_cv, &db->mu);
        }
        if (db->job_head == NULL && db->stop) {
            pthread_mutex_unlock(&db->mu);
            break;
        }
        db_req_t *req = db->job_head;
        db->job_head = req->next;
        if (db->job_head == NULL) {
            db->job_tail = NULL;
        }
        pthread_mutex_unlock(&db->mu);

        db_resp_t *resp = calloc(1, sizeof(*resp));
        if (resp == NULL) {
            free(req);
            continue;
        }
        resp->res.status = worker_execute(&st, &req->job, &resp->res);
        resp->cb = req->cb;
        resp->user = req->user;
        free(req);

        pthread_mutex_lock(&db->mu);
        if (db->resp_tail == NULL) {
            db->resp_head = resp;
        } else {
            db->resp_tail->next = resp;
        }
        db->resp_tail = resp;
        pthread_mutex_unlock(&db->mu);

        /* Wake the loop; the counter accumulates so a lost write is harmless. */
        uint64_t one = 1;
        ssize_t n = write(db->efd, &one, sizeof(one));
        (void)n;
    }

    sqlite3_finalize(st.upsert_user);
    sqlite3_finalize(st.upsert_chat);
    sqlite3_finalize(st.insert_action);
    sqlite3_finalize(st.expire_actions);
    sqlite3_finalize(st.lift_action);
    sqlite3_finalize(st.find_user);
    sqlite3_close(sdb);
    return NULL;
}

/* --- Eventfd handoff ------------------------------------------------------- */

/**
 * @brief Prepare the eventfd read if it is not already armed.
 *
 * @param[in] db DB worker.
 */
static void db_arm_eventfd(db_t *db)
{
    if (db->ops == NULL || db->event_armed || db->stop) {
        return;
    }
    if (db->ops->prep_read_eventfd(db->ops->ctx, &db->event_op, db->efd) == 0) {
        db->event_armed = 1;
    }
}

/**
 * @brief eventfd read completion: drain responses, then re-arm.
 *
 * @param[in] op  The eventfd op (its owner is the @ref db_t).
 * @param[in] res Read result (ignored).
 */
static void on_eventfd(struct op *op, int res)
{
    (void)res;
    db_t *db = op->owner;
    db->event_armed = 0;
    db_pump(db);
    db_arm_eventfd(db); /* re-arm so the next completion is not missed */
}

/* --- Seam ------------------------------------------------------------------ */

/**
 * @brief Adapter implementing @ref db_ops_t::submit for a worker.
 *
 * @param[in] ctx  The @ref db_t.
 * @param[in] job  Job to enqueue (copied).
 * @param[in] cb   Completion callback.
 * @param[in] user Callback context.
 *
 * @return 0 if queued, negative errno on failure.
 */
static int seam_submit(void *ctx, const db_job_t *job, db_cb cb, void *user)
{
    db_t *db = ctx;
    if (db == NULL || job == NULL || cb == NULL) {
        return -EINVAL;
    }
    db_req_t *req = calloc(1, sizeof(*req));
    if (req == NULL) {
        return -ENOMEM;
    }
    req->job = *job;
    req->cb = cb;
    req->user = user;

    pthread_mutex_lock(&db->mu);
    if (db->job_tail == NULL) {
        db->job_head = req;
    } else {
        db->job_tail->next = req;
    }
    db->job_tail = req;
    db->pending++;
    pthread_cond_signal(&db->job_cv);
    pthread_mutex_unlock(&db->mu);
    return 0;
}

/* --- Public API ------------------------------------------------------------ */

db_t *db_create(const db_config_t *cfg)
{
    if (cfg == NULL || cfg->path == NULL) {
        return NULL;
    }
    db_t *db = calloc(1, sizeof(*db));
    if (db == NULL) {
        return NULL;
    }
    (void)snprintf(db->path, sizeof(db->path), "%s", cfg->path);
    db->efd = -1;

    if (pthread_mutex_init(&db->mu, NULL) != 0) {
        free(db);
        return NULL;
    }
    if (pthread_cond_init(&db->job_cv, NULL) != 0) {
        pthread_mutex_destroy(&db->mu);
        free(db);
        return NULL;
    }
    if (pthread_cond_init(&db->ready_cv, NULL) != 0) {
        pthread_cond_destroy(&db->job_cv);
        pthread_mutex_destroy(&db->mu);
        free(db);
        return NULL;
    }
    db->efd = eventfd(0, EFD_CLOEXEC);
    if (db->efd < 0) {
        pthread_cond_destroy(&db->ready_cv);
        pthread_cond_destroy(&db->job_cv);
        pthread_mutex_destroy(&db->mu);
        free(db);
        return NULL;
    }
    db->event_op =
        (op_t){.type = OP_EVENTFD, .owner = db, .on_complete = on_eventfd, .fd = db->efd};

    if (pthread_create(&db->thread, NULL, db_worker, db) != 0) {
        close(db->efd);
        pthread_cond_destroy(&db->ready_cv);
        pthread_cond_destroy(&db->job_cv);
        pthread_mutex_destroy(&db->mu);
        free(db);
        return NULL;
    }

    /* Wait for the worker to report whether open/migrate succeeded. */
    pthread_mutex_lock(&db->mu);
    while (!db->ready) {
        pthread_cond_wait(&db->ready_cv, &db->mu);
    }
    int status = db->init_status;
    pthread_mutex_unlock(&db->mu);

    if (status != 0) {
        /* Signal stop so the worker's early return path is joined cleanly. */
        pthread_mutex_lock(&db->mu);
        db->stop = 1;
        pthread_cond_signal(&db->job_cv);
        pthread_mutex_unlock(&db->mu);
        pthread_join(db->thread, NULL);
        close(db->efd);
        pthread_cond_destroy(&db->ready_cv);
        pthread_cond_destroy(&db->job_cv);
        pthread_mutex_destroy(&db->mu);
        free(db);
        return NULL;
    }
    return db;
}

const db_ops_t *db_ops(db_t *db)
{
    if (db == NULL) {
        return NULL;
    }
    db->seam.submit = seam_submit;
    db->seam.ctx = db;
    return &db->seam;
}

int db_arm(db_t *db, const loop_ops_t *ops)
{
    if (db == NULL || ops == NULL) {
        return -EINVAL;
    }
    db->ops = ops;
    db_arm_eventfd(db);
    return db->event_armed ? 0 : -EIO;
}

void db_pump(db_t *db)
{
    if (db == NULL) {
        return;
    }
    /* Detach the whole response list under the lock, then fire callbacks with the
     * lock released (a callback may submit follow-up jobs). */
    pthread_mutex_lock(&db->mu);
    db_resp_t *list = db->resp_head;
    db->resp_head = NULL;
    db->resp_tail = NULL;
    pthread_mutex_unlock(&db->mu);

    while (list != NULL) {
        db_resp_t *next = list->next;
        list->cb(&list->res, list->user);
        free(list);
        if (db->pending > 0) {
            db->pending--;
        }
        list = next;
    }
}

void db_destroy(db_t *db)
{
    if (db == NULL) {
        return;
    }
    pthread_mutex_lock(&db->mu);
    db->stop = 1;
    pthread_cond_signal(&db->job_cv);
    pthread_mutex_unlock(&db->mu);
    pthread_join(db->thread, NULL);

    db_req_t *req = db->job_head;
    while (req != NULL) {
        db_req_t *next = req->next;
        free(req);
        req = next;
    }
    db_resp_t *resp = db->resp_head;
    while (resp != NULL) {
        db_resp_t *next = resp->next;
        free(resp);
        resp = next;
    }
    close(db->efd);
    pthread_cond_destroy(&db->ready_cv);
    pthread_cond_destroy(&db->job_cv);
    pthread_mutex_destroy(&db->mu);
    free(db);
}

size_t db_pending(const db_t *db)
{
    return db == NULL ? 0 : db->pending;
}
