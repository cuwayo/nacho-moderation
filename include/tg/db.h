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
/** @brief Maximum length of a stored moderation action name (e.g. "ban", "mute"). */
#define DB_ACTION_MAX 24
/** @brief Maximum length of a stored moderation reason. */
#define DB_REASON_MAX 256
/** @brief Maximum length of a stored settings text (welcome/rules template). */
#define DB_TEXT_MAX 4096
/** @brief Maximum length of a stored warn action name ("ban"/"mute"/"kick"). */
#define DB_WARN_ACTION_MAX 8
/** @brief Maximum number of expired actions returned by one expiry sweep. */
#define DB_MAX_EXPIRED 32u
/** @brief Maximum number of warns returned by one warn-list query (one page). */
#define DB_MAX_WARNS 16u

/**
 * @brief The per-chat moderation policy stored in the `chats` row (SPEC section 6).
 *
 * A snapshot of the columns the M5 commands read and write. `DB_JOB_SETTINGS_GET`
 * fills this; when the chat has no row yet, the SPEC defaults are returned
 * (@ref db_settings_defaults) so first-run commands behave predictably.
 */
typedef struct {
    int64_t log_chat_id; /**< Moderation log chat id (0 = disabled). */
    int warn_limit;      /**< Warnings before the auto-action (0 = never auto-act). */
    int welcome_enabled; /**< 1 if the join welcome is enabled. */
    char warn_action[DB_WARN_ACTION_MAX]; /**< "ban" | "mute" | "kick" ("" = default mute). */
    char welcome_text[DB_TEXT_MAX];       /**< Welcome template with placeholders. */
    char rules_text[DB_TEXT_MAX];         /**< Chat rules text ("" if unset). */
} chat_settings_t;

/** @brief One row of the `warns` table (SPEC section 6). */
typedef struct {
    int64_t id;                 /**< warns.id (rowid). */
    int64_t admin_id;           /**< Admin who issued the warning. */
    int64_t created_at;         /**< Unix seconds the warning was created. */
    char reason[DB_REASON_MAX]; /**< Free-text reason (may be ""). */
} db_warn_t;

/* Settings fields addressed by db_job_t.settings_mask (DB_JOB_SETTINGS_SET).
 * Exactly one bit is set per job: the value goes in db_job_t.value (numeric
 * settings) or db_job_t.text (text settings). */
#define DB_SET_WARN_LIMIT 0x01u   /**< db_job_t.value -> chats.warn_limit. */
#define DB_SET_WARN_ACTION 0x02u  /**< db_job_t.text  -> chats.warn_action. */
#define DB_SET_LOG_CHAT 0x04u     /**< db_job_t.value -> chats.log_chat_id. */
#define DB_SET_WELCOME_ON 0x08u   /**< db_job_t.value -> chats.welcome_enabled. */
#define DB_SET_WELCOME_TEXT 0x10u /**< db_job_t.text  -> chats.welcome_text. */
#define DB_SET_RULES_TEXT 0x20u   /**< db_job_t.text  -> chats.rules_text. */

/** @brief Which operation a @ref db_job_t performs. */
typedef enum {
    DB_JOB_UPSERT_USER = 0,       /**< Insert/update a row in `users`. */
    DB_JOB_UPSERT_CHAT,           /**< Insert/update a row in `chats`. */
    DB_JOB_INSERT_ACTION,         /**< Insert an expiring action into `actions`. */
    DB_JOB_EXPIRE_ACTIONS,        /**< Fetch due action rows into @ref db_result_t.expired. */
    DB_JOB_LIFT_ACTION,           /**< Mark one `actions` row as lifted. */
    DB_JOB_FIND_USER_BY_USERNAME, /**< Resolve a username to a user id (`i64`). */
    DB_JOB_WARN_ADD,              /**< Insert a `warns` row; returns the new count in `i64`. */
    DB_JOB_WARN_REMOVE_LAST,      /**< Delete the newest `warns` row for (chat, user). */
    DB_JOB_WARN_LIST,             /**< Fetch a user's `warns` rows into @ref db_result_t.warns. */
    DB_JOB_WARN_RESET,            /**< Delete every `warns` row for (chat, user). */
    DB_JOB_SETTINGS_GET, /**< Read the `chats` policy row into @ref db_result_t.settings. */
    DB_JOB_SETTINGS_SET  /**< Masked `UPDATE chats` for one settings field. */
} db_job_kind_t;

/**
 * @brief One unit of DB work (a tagged, self-contained value).
 *
 * Fixed-size so it can be copied by value through the seam with no ownership
 * transfer: the caller may reuse or discard @p job immediately after submit.
 * Only the fields relevant to @ref kind are read.
 *
 * The struct is deliberately flat (no union): the seam, the worker queue and the
 * tests all copy it by value, and at the target <=1000 updates/s the extra bytes
 * are an insignificant memcpy. See @ref db_result_t for the matching rationale.
 */
typedef struct {
    db_job_kind_t kind;             /**< Operation to perform. */
    int64_t id;                     /**< user_id (users) | chat_id (chats) | action id (lift). */
    int64_t last_seen;              /**< Unix seconds; users only. */
    int64_t user_id;                /**< actions.user_id (the target). */
    int64_t admin_id;               /**< actions.admin_id (the actor). */
    int64_t created_at;             /**< actions.created_at (Unix seconds). */
    int64_t expires_at;             /**< actions.expires_at (Unix seconds; <=0 = permanent). */
    int64_t now_unix;               /**< Cutoff for DB_JOB_EXPIRE_ACTIONS. */
    int64_t value;                  /**< Numeric setting value (DB_JOB_SETTINGS_SET). */
    unsigned settings_mask;         /**< Which chats column to write (DB_SET_* bit). */
    char username[DB_USERNAME_MAX]; /**< users.username, or the lookup key. */
    char first_name[DB_FIRST_NAME_MAX]; /**< users.first_name. */
    char title[DB_TITLE_MAX];           /**< chats.title. */
    char chat_type[DB_CHAT_TYPE_MAX];   /**< chats.type. */
    char action[DB_ACTION_MAX];         /**< actions.action ("ban"/"mute"). */
    char reason[DB_REASON_MAX];         /**< actions.reason / warns.reason. */
    char text[DB_TEXT_MAX];             /**< Text setting value (DB_JOB_SETTINGS_SET). */
} db_job_t;

/** @brief One expired action returned by a @ref DB_JOB_EXPIRE_ACTIONS sweep. */
typedef struct {
    int64_t id;                 /**< actions.id (for DB_JOB_LIFT_ACTION). */
    int64_t chat_id;            /**< Chat the action applies to. */
    int64_t user_id;            /**< User the action applies to. */
    char action[DB_ACTION_MAX]; /**< Action name ("ban"/"mute"). */
} db_expired_action_t;

/**
 * @brief Outcome of a DB job.
 *
 * Flat and fixed-size, like @ref db_job_t (fields are ordered scalars-first and
 * arrays-last to avoid struct padding), and the whole value is copied by value
 * once per completed job (the worker heap-allocates it, the loop copies it out).
 */
typedef struct {
    int status;             /**< 0 on success, negative @ref err_class_t otherwise. */
    int64_t i64;            /**< Scalar result (user id / warn count, else 0). */
    unsigned expired_count; /**< Entries in @ref expired (DB_JOB_EXPIRE_ACTIONS only). */
    unsigned warn_count;    /**< Entries in @ref warns (DB_JOB_WARN_LIST only). */
    db_expired_action_t expired[DB_MAX_EXPIRED]; /**< Due actions from an expiry sweep. */
    db_warn_t warns[DB_MAX_WARNS];               /**< Rows from a warn-list query. */
    chat_settings_t settings;                    /**< Policy row (DB_JOB_SETTINGS_GET only). */
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

/**
 * @brief Fill @p out with the SPEC section 6 defaults for a chat's policy.
 *
 * Used when a chat has no `chats` row yet (`DB_JOB_SETTINGS_GET` returns these)
 * and by the settings module's cache fallback, so first-run commands see
 * `warn_limit=3`, `warn_action="mute"`, welcome disabled, no log channel.
 *
 * @param[out] out Settings to fill (zeroed first).
 * @thread_safety Safe (writes only @p out).
 */
void db_settings_defaults(chat_settings_t *out);

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
