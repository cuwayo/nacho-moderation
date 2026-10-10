/**
 * @file tg_types.h
 * @brief Parsed Telegram Update/Message/User/Chat structs (SPEC section 4.1, 6).
 *
 * The update parser turns the `getUpdates` reply body into bounded, plain-old-data
 * structs the router and handlers work with. It is the only module that knows the
 * Telegram JSON shape for updates: yyjson stays inside tg_types.c, and every field
 * is copied into a fixed-capacity buffer so callers never hold a pointer into the
 * response body (which the API pool frees after the callback, SPEC section 9).
 *
 * Size caps follow SPEC section 9: message text <= 4096 characters, and every id
 * is a signed 64-bit integer (supergroup ids are negative, e.g. -1001234567890).
 * Missing, null and unknown fields are tolerated: they leave the corresponding
 * struct member zeroed rather than failing the parse.
 *
 * @ingroup router
 */
#ifndef TG_TG_TYPES_H
#define TG_TG_TYPES_H

#include <stddef.h>
#include <stdint.h>

/** @brief Maximum number of new chat members captured from one join message. */
#define TG_MAX_NEW_MEMBERS 16u

/** @brief A Telegram user (subset used by the bot). */
typedef struct {
    int64_t id;           /**< User id (unique, never 0 for a real user). */
    int is_bot;           /**< 1 if the account is a bot. */
    char first_name[128]; /**< First name (UTF-8, NUL-terminated). */
    char username[64];    /**< Username without the leading at-sign ("" if none). */
} tg_user_t;

/** @brief A Telegram chat (subset used by the bot). */
typedef struct {
    int64_t id;      /**< Chat id (negative for groups/supergroups). */
    char type[24];   /**< "private", "group", "supergroup" or "channel". */
    char title[256]; /**< Chat title (groups/supergroups; "" for private). */
} tg_chat_t;

/** @brief A Telegram message (subset used by the bot). */
typedef struct {
    int64_t message_id;          /**< Per-chat message id. */
    int64_t date;                /**< Unix time the message was sent. */
    tg_user_t from;              /**< Sender (zeroed for service messages). */
    tg_chat_t chat;              /**< Chat the message belongs to. */
    char text[4097];             /**< Message text (UTF-8, NUL-terminated; "" if none). */
    int has_reply;               /**< 1 if the message replies to another message. */
    int64_t reply_to_message_id; /**< Id of the replied-to message (0 if none). */
    tg_user_t reply_from;        /**< Sender of the replied-to message (valid if has_reply). */
    unsigned new_member_count;   /**< Entries in @ref new_members. */
    tg_user_t new_members[TG_MAX_NEW_MEMBERS]; /**< Users added by a join message. */
    int is_service; /**< 1 for a service message (no sender, e.g. joins/pins). */
} tg_message_t;

/** @brief Which update variant an @ref tg_update_t carries. */
typedef enum {
    TG_UPD_UNKNOWN = 0,    /**< A recognized shape the bot does not handle. */
    TG_UPD_MESSAGE,        /**< A new message. */
    TG_UPD_EDITED_MESSAGE, /**< An edited message. */
    TG_UPD_CALLBACK_QUERY, /**< An inline-keyboard button press. */
    TG_UPD_CHAT_MEMBER,    /**< A chat member's status changed. */
    TG_UPD_MY_CHAT_MEMBER  /**< The bot's own status in a chat changed. */
} tg_update_kind_t;

/** @brief One parsed update. */
typedef struct {
    int64_t update_id;     /**< Monotonic update id used for the poll offset. */
    tg_update_kind_t kind; /**< Which variant the populated fields belong to. */

    /* TG_UPD_MESSAGE / TG_UPD_EDITED_MESSAGE */
    tg_message_t message; /**< Parsed message (message/edited_message). */

    /* TG_UPD_CALLBACK_QUERY */
    char callback_id[64];        /**< Opaque callback query id (for answerCallbackQuery). */
    tg_user_t callback_from;     /**< User who pressed the button. */
    char callback_data[65];      /**< Button payload (Telegram caps it at 64 bytes). */
    int callback_has_message;    /**< 1 if the callback carries a message. */
    int64_t callback_message_id; /**< Message carrying the keyboard (0 if none). */
    tg_chat_t callback_chat;     /**< Chat of the callback's message (valid if has_message). */

    /* TG_UPD_CHAT_MEMBER / TG_UPD_MY_CHAT_MEMBER */
    tg_chat_t member_chat; /**< Chat whose membership changed. */
    tg_user_t member_from; /**< User who made the change (zeroed if unknown). */
    tg_user_t member_user; /**< The user whose membership changed. */
    char old_status[24];   /**< Previous ChatMember status ("" if unknown). */
    char new_status[24];   /**< New ChatMember status. */
} tg_update_t;

/** @brief Per-update visitor; return non-zero to stop iteration early. */
typedef int (*tg_update_cb)(const tg_update_t *update, void *ctx);

/**
 * @brief Parse a single Update JSON object into @p out.
 *
 * Accepts the raw object (not the `{"ok":true,"result":[...]}` envelope). Unknown
 * top-level keys are ignored; a malformed or non-object input fails.
 *
 * @param[in]  json JSON bytes of one update object.
 * @param[in]  len  Length of @p json.
 * @param[out] out  Receives the parsed update (zeroed before parsing).
 *
 * @return 0 on success, -ERR_PARSE on malformed JSON or a non-object, -EINVAL on
 *         NULL arguments.
 * @ownership @p out is caller-owned; no heap allocation, no pointers into @p json.
 * @thread_safety Safe (no shared state).
 */
int tg_update_parse(const char *json, size_t len, tg_update_t *out);

/**
 * @brief Parse a `getUpdates` reply body, invoking @p cb once per update.
 *
 * Accepts the standard `{"ok":true,"result":[...]}` envelope. Each element is
 * parsed with @ref tg_update_parse and passed to @p cb; a non-zero return from
 * @p cb stops iteration (the count reflects the updates delivered so far).
 *
 * @param[in]  json  Reply body.
 * @param[in]  len   Length of @p json.
 * @param[in]  cb    Visitor invoked per update (never NULL).
 * @param[in]  ctx   Opaque pointer passed to @p cb.
 * @param[out] count Receives the number of updates delivered (may be NULL).
 *
 * @return 0 on success (including an empty batch), -ERR_PARSE on malformed JSON,
 *         a missing `ok=true` or a non-array `result`, -EINVAL on NULL arguments.
 * @ownership @p json is borrowed for the duration of the call only.
 * @thread_safety Safe (no shared state).
 */
int tg_updates_parse(const char *json, size_t len, tg_update_cb cb, void *ctx, unsigned *count);

#endif /* TG_TG_TYPES_H */
