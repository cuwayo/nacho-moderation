/* tables.h - shared enums for the SSOT (single source of truth) tables.
 *
 * SPEC-ADDENDUM-SSOT S2-S4. These enums are the C spelling of the tokens used in
 * the .def tables (commands.def, config.def, and the other S4 tables). They are
 * deliberately small and stable: adding a member is a table change, not a code
 * change, and every derived artifact is generated from or checked against the
 * tables.
 *
 * @file tables.h
 * @brief Shared enumerations referenced by the X-macro .def tables.
 * @ingroup util
 */
#ifndef NM_TABLES_H
#define NM_TABLES_H

/** @brief Minimum caller role for a command (SPEC section 5.1). */
typedef enum {
    ROLE_ANYONE = 0, /**< Any member of the chat. */
    ROLE_ADMIN,      /**< A Telegram administrator (or owner/sudo). */
    ROLE_OWNER,      /**< The chat creator only. */
    ROLE_SUDO        /**< A bot-level sudo user from config. */
} role_t;

/** @brief Telegram admin right a caller or the bot must hold (SPEC section 5.1). */
typedef enum {
    RIGHT_NONE = 0,   /**< No specific right required. */
    RIGHT_RESTRICT,   /**< can_restrict_members. */
    RIGHT_DELETE,     /**< can_delete_messages. */
    RIGHT_PIN,        /**< can_pin_messages. */
    RIGHT_PROMOTE,    /**< can_promote_members. */
    RIGHT_CHANGE_INFO /**< can_change_info. */
} right_t;

/** @brief Bitmask bit for a @ref right_t value (rights are held as an OR of these). */
#define RIGHT_BIT(r) (1u << (unsigned)(r))

/* Command flags (CMDF_*), OR-ed in the commands.def flags column. */
#define CMDF_TARGET 0x01u         /**< Takes a target user argument. */
#define CMDF_REPLY_REQUIRED 0x02u /**< Must be used as a reply to a message. */
#define CMDF_DURATION 0x04u       /**< Accepts an optional duration argument. */
#define CMDF_REASON 0x08u         /**< Accepts trailing free-text reason. */
#define CMDF_GROUP_ONLY 0x10u     /**< Rejected in private chats. */
#define CMDF_HIDDEN 0x20u         /**< Omitted from /help and setMyCommands. */

/** @brief Configuration value type (SPEC-ADDENDUM-SSOT S3). */
typedef enum {
    CFG_STR = 0,  /**< UTF-8 string. */
    CFG_INT,      /**< Signed integer. */
    CFG_BOOL,     /**< Boolean true/false. */
    CFG_DURATION, /**< Duration literal, e.g. "30s". */
    CFG_I64_LIST, /**< Comma-separated 64-bit integers. */
    CFG_ENUM      /**< One of a fixed set from config_enums.def. */
} cfg_type_t;

/** @brief Structured log level, ordered from most to least severe. */
typedef enum {
    LOG_ERROR = 0, /**< A failure that needs attention. */
    LOG_WARN,      /**< A recoverable anomaly. */
    LOG_INFO,      /**< Normal lifecycle events. */
    LOG_DEBUG      /**< Verbose diagnostics (debug builds). */
} log_level_t;

/** @brief Rate-limit bucket class for a Bot API method (SPEC section 4.2). */
typedef enum {
    BUCKET_GLOBAL = 0, /**< Global 25 req/s limit. */
    BUCKET_PER_CHAT,   /**< 15 req/min per chat (messages). */
    BUCKET_BULK        /**< 1 req/s per chat (bulk operations). */
} bucket_t;

#endif /* NM_TABLES_H */
