/**
 * @file db.h
 * @brief SQLite persistence on a dedicated worker thread (SPEC section 4.4, ADR 0003).
 *
 * All SQLite access happens on one DB worker thread. The loop submits @ref db_job_t
 * jobs through the injectable @ref db_ops_t seam; the worker executes them with
 * prepared statements, pushes the results to a response queue and writes an
 * `eventfd`. The loop reads that eventfd through io_uring (@ref OP_EVENTFD) and
 * @ref db_pump dispatches the completion callbacks on the loop thread.
 *
 * Consequences (ADR 0003):
 *  - the main thread never touches SQLite or its file descriptors;
 *  - statements are prepared once and reused; SQL is never string-concatenated;
 *  - the worker signals the loop only through the eventfd, so no lock is needed
 *    on loop-owned state.
 *
 * Tests use the @ref db_ops_t seam with a fake (no SQLite); the real worker is
 * covered by a test that opens a temporary database file.
 *
 * @ingroup db
 */
#ifndef TG_DB_H
#define TG_DB_H

#include <stddef.h>
#include <stdint.h>

#include "tg/loop.h"

/** @brief Maximum length of a stored username (matches tg_user_t). */
#define DB_USERNAME_MAX 64
/** @brief Maximum length of a stored first name (matches tg_user_t). */
#define DB_FIRST_NAME_MAX 128
/** @brief Maximum length of a stored chat title (matches tg_chat_t). */
#define DB_TITLE_MAX 256
/** @brief Maximum length of a stored chat type. */
#define DB_CHAT_TYPE_MAX 24

/** @brief Which operation a @ref db_job_t performs. */
typedef enum {
    DB_JOB_UPSERT_USER = 0, /**< Insert/update a row in `users`. */
    DB_JOB_UPSERT_CHAT      /**< Insert/update a row in `chats`. */
} db_job_kind_t;

/**
 * @brief One unit of DB work (a tagged, self-contained value).
 *
 * Fixed-size so it can be copied by value through the seam with no ownership
 * transfer: the caller may reuse or discard @p job immediately after submit.
 */
typedef struct {
    db_job_kind_t kind;                 /**< Operation to perform. */
    int64_t id;                         /**< user_id (users) or chat_id (chats). */
    int64_t last_seen;                  /**< Unix seconds; users only. */
    char username[DB_USERNAME_MAX];     /**< users.username. */
    char first_name[DB_FIRST_NAME_MAX]; /**< users.first_name. */
    char title[DB_TITLE_MAX];           /**< chats.title. */
    char chat_type[DB_CHAT_TYPE_MAX];   /**< chats.type. */
} db_job_t;

/** @brief Outcome of a DB job. */
typedef struct {
    int status;  /**< 0 on success, negative @ref err_class_t otherwise. */
    int64_t i64; /**< Scalar result (unused by the M3 upserts). */
} db_result_t;

/** @brief Job completion callback; runs on the loop thread. */
typedef void (*db_cb)(const db_result_t *res, void *ctx);

/** @brief Injectable DB seam so logic modules never touch SQLite directly (A3). */
typedef struct {
    /**
     * @brief Queue @p job for execution.
     *
     * @param[in] ctx  Implementation state (a @ref db_t).
     * @param[in] job  Job (copied; may be freed by the caller after return).
     * @param[in] cb   Completion callback (never NULL).
     * @param[in] user Opaque pointer passed to @p cb.
     *
     * @return 0 if queued, negative errno on failure (-EINVAL, -ENOMEM).
     */
    int (*submit)(void *ctx, const db_job_t *job, db_cb cb, void *user);
    void *ctx; /**< Implementation state. */
} db_ops_t;

/** @brief Configuration for a DB worker. */
typedef struct {
    const char *path; /**< SQLite file path (":" memory not supported across threads). */
} db_config_t;

/** @brief The DB worker (opaque; see db.c). */
typedef struct db db_t;

/**
 * @brief Open the database, run migrations, and start the worker thread.
 *
 * The worker opens SQLite (WAL, `synchronous=NORMAL`, `foreign_keys=ON`,
 * `busy_timeout=5000`), applies the schema migration, and then processes jobs.
 * This call blocks only until the worker reports readiness (startup, before the
 * event loop runs — the same pattern as @ref resolver_join).
 *
 * @param[in] cfg Configuration (path copied).
 *
 * @return A new DB worker, or NULL on failure (bad arguments, open/migrate error).
 * @ownership Caller must call db_destroy().
 * @thread_safety Call from the loop thread at startup.
 */
db_t *db_create(const db_config_t *cfg);

/**
 * @brief Return the injectable seam bound to @p db.
 *
 * @param[in] db DB worker.
 *
 * @return Pointer to the seam (owned by @p db), or NULL.
 * @ownership Owned by @p db; do not free.
 * @thread_safety Loop thread only.
 */
const db_ops_t *db_ops(db_t *db);

/**
 * @brief Arm the eventfd read that wakes the loop when a job completes.
 *
 * Prepares an @ref OP_EVENTFD read on the worker's eventfd; the next
 * @ref loop_run_once submits it, and its completion drains the response queue and
 * re-arms. Call once at startup, before entering the loop.
 *
 * @param[in] db  DB worker.
 * @param[in] ops Loop vtable.
 *
 * @return 0 if armed, negative errno on failure.
 * @thread_safety Loop thread only.
 */
int db_arm(db_t *db, const loop_ops_t *ops);

/**
 * @brief Dispatch completed job callbacks queued by the worker.
 *
 * Call once per loop iteration (and from the eventfd completion). Each callback
 * runs exactly once, on the loop thread.
 *
 * @param[in] db DB worker.
 * @thread_safety Loop thread only.
 */
void db_pump(db_t *db);

/**
 * @brief Stop the worker, join it, and release every resource.
 *
 * Any jobs still queued are abandoned without invoking their callbacks (shutdown
 * path; graceful drain is completed in M7).
 *
 * @param[in] db DB worker (may be NULL).
 * @thread_safety Loop thread only.
 */
void db_destroy(db_t *db);

/**
 * @brief Number of jobs queued but not yet completed.
 *
 * @param[in] db DB worker.
 *
 * @return Outstanding job count.
 * @thread_safety Loop thread only.
 */
size_t db_pending(const db_t *db);

#endif /* TG_DB_H */
