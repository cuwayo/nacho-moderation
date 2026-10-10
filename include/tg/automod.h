/**
 * @file automod.h
 * @brief Automated moderation: locks, blacklist, and antiflood (SPEC section 5.3).
 *
 * Automod runs on every group message, before command dispatch, in the order the
 * SPEC fixes: **locks -> blacklist -> antiflood**. It is driven by the router
 * (which owns the per-chat settings and administrator caches) but keeps its own
 * state: the antiflood sliding window lives in memory, keyed by
 * `(chat_id, user_id)` and advanced by the injected monotonic clock.
 *
 * The module never blocks and never touches the network or SQLite directly: it
 * uses the @ref tg_api_ops_t and @ref db_ops_t seams. The message classifiers
 * (@ref automod_locked_content, @ref automod_blacklist_match) are pure and unit
 * tested; the sliding-window arithmetic (@ref automod_flood_track) is pure and
 * driven by an injected clock.
 *
 * @ingroup automod
 */
#ifndef TG_AUTOMOD_H
#define TG_AUTOMOD_H

#include <stddef.h>
#include <stdint.h>

#include "tg/db.h"
#include "tg/perms.h"
#include "tg/settings.h"
#include "tg/tg_api.h"
#include "tg/tg_types.h"
#include "tg/util.h"

/**
 * @brief Lock-type bits for the `chats.locks` bitmask (SPEC section 6).
 *
 * Generated from @c src/locks.def so the argument names, bit indices and docs
 * cannot drift from the single source of truth (SSOT S4). Each `LOCK_<TYPE>` is
 * an unsigned single-bit mask; OR them into the `chats.locks` column. A lock
 * mask is a plain `unsigned`.
 *
 * @name Lock-type bits
 * @{
 */
/*! @cond */ /* X-macro expansion over locks.def (SSOT S4); each row becomes one constant. */
#define L(id, name, bit, brief) /** @brief Lock bit: brief */                                      \
    static const unsigned LOCK_##id = (1u << (bit));
#include "locks.def"
#undef L
/*! @endcond */
/** @} */

/** @brief Maximum number of message ids retained in one antiflood window. */
#define AUTOMOD_FLOOD_SLOTS 16u

/**
 * @brief One user's antiflood sliding window in a chat.
 *
 * A fixed ring of the most recent message timestamps (monotonic ms) and their
 * ids. Old entries fall out once they are older than the chat's window.
 */
typedef struct {
    uint64_t ts[AUTOMOD_FLOOD_SLOTS];    /**< Monotonic ms each message arrived. */
    int64_t msg_id[AUTOMOD_FLOOD_SLOTS]; /**< Message id of each tracked message. */
    unsigned count;                      /**< Entries currently in the window. */
} flood_window_t;

/** @brief Configuration for the automod service. All pointers must outlive it. */
typedef struct {
    const tg_api_ops_t *api;      /**< API seam (delete / restrict). */
    const db_ops_t *db;           /**< DB seam (blacklist, warns, actions). */
    const settings_t *settings;   /**< Settings service (policy + log channel). */
    perms_t *perms;               /**< Permission engine (skip admins/sudo). */
    const clock_ops_t *clock;     /**< Monotonic clock (flood window). */
    const wall_clock_ops_t *wall; /**< Wall clock (mute until_date). */
    int64_t bot_id;               /**< The bot's own user id. */
    unsigned flood_mute_seconds;  /**< Mute duration for an antiflood trip (0 = 300). */
} automod_config_t;

/** @brief The automod service (opaque; see automod.c). */
typedef struct automod automod_t;

/**
 * @brief Create the automod service.
 *
 * @param[in] cfg Configuration (copied; pointers must outlive the service).
 *
 * @return A new service, or NULL on failure.
 * @ownership Caller must call automod_destroy().
 * @thread_safety Loop thread only.
 */
automod_t *automod_create(const automod_config_t *cfg);

/**
 * @brief Destroy the service and its flood state.
 *
 * @param[in] a Service (may be NULL).
 * @thread_safety Loop thread only.
 */
void automod_destroy(automod_t *a);

/**
 * @brief Run the automod pipeline for one group message (SPEC section 5.3).
 *
 * Fetches the chat policy and the administrator list (both cached), skips
 * administrators/owner/sudo and service messages, then applies locks, blacklist
 * and antiflood in that order. Deletions, warns and mutes are issued through the
 * API/DB seams and never block.
 *
 * @param[in] a   Service.
 * @param[in] msg Message to evaluate (borrowed; copied where it must outlive).
 *
 * @return 0 if handled (including "nothing to do"), negative errno on a fatal
 *         argument error.
 * @ownership @p msg is borrowed; the service copies what it keeps.
 * @thread_safety Loop thread only.
 */
int automod_handle_message(automod_t *a, const tg_message_t *msg);

/**
 * @brief Return the bitmask of lock types a message violates (pure).
 *
 * Classifies @p msg against @p locks (a lock mask) using the parsed
 * content flags and entities. A message with no matching lock returns 0. The
 * `bots` lock matches any message whose sender is a bot; `commands` matches a
 * leading bot command; `invites` matches a Telegram invite link in the text.
 *
 * @param[in] locks Chat lock mask.
 * @param[in] msg   Message to classify.
 *
 * @return The matching lock bits (0 if none).
 * @thread_safety Safe (no shared state).
 */
unsigned automod_locked_content(unsigned locks, const tg_message_t *msg);

/**
 * @brief Report whether @p text matches one blacklist pattern (pure).
 *
 * A plain pattern matches as a case-insensitive substring; a regex pattern is
 * compiled with PCRE2 and matched with match/depth limits so a hostile pattern
 * cannot cause catastrophic backtracking (SPEC section 10).
 *
 * @param[in] pattern  Pattern text.
 * @param[in] is_regex 1 to treat @p pattern as a regular expression.
 * @param[in] text     Text to test (may be NULL/empty).
 *
 * @return 1 on a match, 0 otherwise (including an invalid regex).
 * @thread_safety Safe (no shared state; PCRE2 match contexts are local).
 */
int automod_blacklist_match(const char *pattern, int is_regex, const char *text);

/**
 * @brief Add one message to a sliding window, dropping expired entries (pure).
 *
 * @param[in,out] w         Window to update.
 * @param[in]     now_ms    Current monotonic time in milliseconds.
 * @param[in]     msg_id    Message id to record.
 * @param[in]     window_ms Window length in milliseconds.
 *
 * @return The number of entries in the window after the update (>= 1).
 * @thread_safety Safe (operates only on @p w).
 */
unsigned automod_flood_track(flood_window_t *w, uint64_t now_ms, int64_t msg_id,
                             uint64_t window_ms);

#endif /* TG_AUTOMOD_H */
