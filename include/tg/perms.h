/**
 * @file perms.h
 * @brief Permission model and per-chat administrator cache (SPEC section 5.1).
 *
 * Two responsibilities:
 *  1. **Pure decision logic** (@ref perms_check, @ref perms_target_ok): given a
 *     caller's role/rights and a command, decide whether the command may run.
 *     These are exhaustively unit-tested against the A5.1 matrix and never do I/O.
 *  2. **Admin cache** (@ref perms_admins_get): fetch `getChatAdministrators` for a
 *     chat, cache the result for @ref PERMS_TTL_MS, and invalidate it on a
 *     `chat_member` update or `/reload`. Time comes from the injected clock.
 *
 * Roles are the bot's own view: the Telegram creator is `owner`, a Telegram
 * administrator is `admin`, everyone else is `member`, and configured user ids
 * are `sudo` (which bypasses the caller-right check). The bot's own rights in the
 * chat are checked separately so the bot can reply "I lack rights" rather than
 * attempting an action Telegram would reject (SPEC section 5.1).
 *
 * @ingroup perms
 */
#ifndef TG_PERMS_H
#define TG_PERMS_H

#include <stddef.h>
#include <stdint.h>

#include "commands.h"
#include "tables.h"
#include "tg/tg_api.h"
#include "tg/util.h"

/** @brief Admin-cache time-to-live (SPEC section 5.1: 10 minutes). */
#define PERMS_TTL_MS (10ULL * 60ULL * 1000ULL)

/** @brief Maximum administrators cached per chat. */
#define PERMS_MAX_ADMINS 64u

/** @brief A caller's effective role and held rights in a chat. */
typedef struct {
    role_t role;     /**< owner | admin | member (sudo handled via @ref is_sudo). */
    unsigned rights; /**< Bitmask of RIGHT_BIT(right_t) held by the caller. */
    int is_sudo;     /**< 1 if the caller is a configured sudo user. */
} perms_subject_t;

/** @brief One administrator cached for a chat. */
typedef struct {
    int64_t user_id; /**< Administrator user id. */
    role_t role;     /**< ROLE_OWNER for the creator, else ROLE_ADMIN. */
    unsigned rights; /**< Bitmask of RIGHT_BIT(right_t) the admin holds. */
} perms_admin_t;

/** @brief Cached administrators of one chat. */
typedef struct {
    int64_t chat_id;                        /**< Chat the list belongs to. */
    unsigned count;                         /**< Number of entries in @ref admins. */
    perms_admin_t admins[PERMS_MAX_ADMINS]; /**< Administrator entries. */
} perms_admins_t;

/** @brief Admin-list callback; runs on the loop thread. */
typedef void (*perms_admins_cb)(const perms_admins_t *admins, int status, void *ctx);

/** @brief Subject-resolution callback; runs on the loop thread. */
typedef void (*perms_subject_cb)(const perms_subject_t *subject, int status, void *ctx);

/** @brief The permission engine (opaque; see perms.c). */
typedef struct perms perms_t;

/** @brief Configuration for the permission engine. */
typedef struct {
    const clock_ops_t *clock; /**< Injected clock (admin-cache TTL). */
    const tg_api_ops_t *api;  /**< API seam for getChatAdministrators. */
    int64_t bot_id;           /**< The bot's own user id. */
    const int64_t *sudo_ids;  /**< Configured sudo user ids (may be NULL). */
    size_t sudo_count;        /**< Number of sudo ids. */
} perms_config_t;

/**
 * @brief Report whether @p subject may run @p cmd in the current chat.
 *
 * Pure logic. Rules (SPEC section 5.1):
 *  - owner and sudo bypass the caller-right check;
 *  - admin must hold @c cmd->user_right;
 *  - member is rejected for any command whose @c min_role is not @c ROLE_ANYONE;
 *  - the bot must hold @c cmd->bot_right (@p bot_has_right).
 *
 * @param[in] subject       Caller's role and rights.
 * @param[in] cmd           Command descriptor (NULL = always allowed).
 * @param[in] bot_has_right 1 if the bot holds @c cmd->bot_right in the chat.
 *
 * @return 0 if allowed, -ERR_PERM if the caller lacks permission, -ERR_TG_API if
 *         only the bot lacks the right (so the caller can say "I lack rights").
 * @thread_safety Safe (no shared state).
 */
int perms_check(const perms_subject_t *subject, const cmd_desc_t *cmd, int bot_has_right);

/**
 * @brief Report whether @p target may be acted on.
 *
 * Never allows acting on the chat owner, another administrator, or the bot itself
 * (SPEC section 5.1).
 *
 * @param[in] target     Target's role in the chat.
 * @param[in] target_id  Target user id.
 * @param[in] bot_id     The bot's own user id.
 *
 * @return 0 if the target may be acted on, -ERR_PERM otherwise.
 * @thread_safety Safe (no shared state).
 */
int perms_target_ok(role_t target, int64_t target_id, int64_t bot_id);

/**
 * @brief Create the permission engine.
 *
 * @param[in] cfg Configuration (pointers must outlive the engine).
 *
 * @return A new engine, or NULL on failure.
 * @ownership Caller must call perms_destroy().
 * @thread_safety Loop thread only.
 */
perms_t *perms_create(const perms_config_t *cfg);

/**
 * @brief Destroy the engine and its admin cache.
 *
 * @param[in] p Engine (may be NULL).
 * @thread_safety Loop thread only.
 */
void perms_destroy(perms_t *p);

/**
 * @brief Report whether @p user_id is a configured sudo user.
 *
 * @param[in] p       Engine.
 * @param[in] user_id User to test.
 *
 * @return 1 if sudo, else 0.
 * @thread_safety Loop thread only.
 */
int perms_is_sudo(const perms_t *p, int64_t user_id);

/**
 * @brief Fetch (or reuse) the cached administrator list for @p chat_id.
 *
 * On a cache hit @p cb runs synchronously; on a miss it issues
 * `getChatAdministrators`, caches the result for @ref PERMS_TTL_MS, and invokes
 * @p cb when the reply arrives.
 *
 * @param[in] p       Engine.
 * @param[in] chat_id Chat to query.
 * @param[in] cb      Callback receiving the list and status (never NULL).
 * @param[in] ctx     Opaque pointer passed to @p cb.
 *
 * @return 0 if the request was started or resolved synchronously, negative errno
 *         if it could not be started (the callback does not run then).
 * @ownership @p ctx must stay valid until @p cb runs; the @ref perms_admins_t
 *            passed to @p cb is valid only for the duration of the call.
 * @thread_safety Loop thread only.
 */
int perms_admins_get(perms_t *p, int64_t chat_id, perms_admins_cb cb, void *ctx);

/**
 * @brief Derive @p user_id's subject from an administrator list (pure).
 *
 * The creator (the entry flagged @c ROLE_OWNER) is `owner`; other listed users
 * are `admin`; anyone else is `member`. Configured sudo users are flagged so they
 * bypass the caller-right check.
 *
 * @param[in]  p      Engine (for the sudo list).
 * @param[in]  admins Administrator list (may be NULL = none).
 * @param[in]  user_id User to classify.
 * @param[out] out    Receives the subject.
 *
 * @return 0 on success, -EINVAL on bad arguments.
 * @thread_safety Safe (no shared state).
 */
int perms_subject_from_admins(const perms_t *p, const perms_admins_t *admins, int64_t user_id,
                              perms_subject_t *out);

/**
 * @brief Resolve @p user_id's role and rights in @p chat_id.
 *
 * Convenience over @c perms_admins_get + @c perms_subject_from_admins: it
 * fetches the admin list (cached) and invokes @p cb with the derived subject.
 *
 * @param[in] p       Engine.
 * @param[in] chat_id Chat to resolve in.
 * @param[in] user_id User to resolve.
 * @param[in] cb      Callback receiving the subject and status (never NULL).
 * @param[in] ctx     Opaque pointer passed to @p cb.
 *
 * @return 0 if the request was started or resolved synchronously, negative errno
 *         if it could not be started (the callback does not run then).
 * @ownership @p ctx must stay valid until @p cb runs.
 * @thread_safety Loop thread only.
 */
int perms_subject_get(perms_t *p, int64_t chat_id, int64_t user_id, perms_subject_cb cb, void *ctx);

/**
 * @brief Invalidate the cached admin list for @p chat_id.
 *
 * Called on a `chat_member` update and on `/reload` (SPEC section 5.1).
 *
 * @param[in] p       Engine.
 * @param[in] chat_id Chat whose cache is dropped.
 * @thread_safety Loop thread only.
 */
void perms_invalidate(perms_t *p, int64_t chat_id);

#endif /* TG_PERMS_H */
