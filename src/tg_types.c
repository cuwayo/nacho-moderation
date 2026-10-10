/**
 * @file tg_types.c
 * @brief Update/Message/User/Chat parsing from Telegram JSON (SPEC section 4.1, 6).
 *
 * yyjson is confined to this translation unit. Every value is copied into the
 * fixed-capacity members of the structs in tg_types.h, so nothing the caller
 * receives points into the response buffer.
 *
 * @ingroup router
 */
#include "tg/tg_types.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <yyjson.h>

#include "tg/errors.h"

/**
 * @brief Copy a JSON string value into a fixed buffer, NUL-terminated.
 *
 * A missing, null or non-string value leaves @p dst as an empty string.
 *
 * @param[out] dst Destination buffer.
 * @param[in]  cap Capacity of @p dst in bytes.
 * @param[in]  v   JSON value (may be NULL).
 */
static void copy_str(char *dst, size_t cap, yyjson_val *v)
{
    dst[0] = '\0';
    if (v == NULL || !yyjson_is_str(v)) {
        return;
    }
    const char *s = yyjson_get_str(v);
    if (s == NULL) {
        return;
    }
    (void)snprintf(dst, cap, "%s", s);
}

/**
 * @brief Read an integer member of @p obj, or 0 when absent/non-integer.
 *
 * @param[in] obj JSON object.
 * @param[in] key Member name.
 *
 * @return The value, or 0.
 */
static int64_t get_i64(yyjson_val *obj, const char *key)
{
    yyjson_val *v = yyjson_obj_get(obj, key);
    return (v != NULL && yyjson_is_int(v)) ? (int64_t)yyjson_get_sint(v) : 0;
}

/**
 * @brief Parse a Telegram User object.
 *
 * @param[in]  v   JSON object (may be NULL).
 * @param[out] out Receives the parsed user (zeroed first).
 */
static void parse_user(yyjson_val *v, tg_user_t *out)
{
    memset(out, 0, sizeof(*out));
    if (v == NULL || !yyjson_is_obj(v)) {
        return;
    }
    out->id = get_i64(v, "id");
    yyjson_val *is_bot = yyjson_obj_get(v, "is_bot");
    out->is_bot = is_bot != NULL && yyjson_get_bool(is_bot);
    copy_str(out->first_name, sizeof(out->first_name), yyjson_obj_get(v, "first_name"));
    copy_str(out->username, sizeof(out->username), yyjson_obj_get(v, "username"));
}

/**
 * @brief Parse a Telegram Chat object.
 *
 * @param[in]  v   JSON object (may be NULL).
 * @param[out] out Receives the parsed chat (zeroed first).
 */
static void parse_chat(yyjson_val *v, tg_chat_t *out)
{
    memset(out, 0, sizeof(*out));
    if (v == NULL || !yyjson_is_obj(v)) {
        return;
    }
    out->id = get_i64(v, "id");
    copy_str(out->type, sizeof(out->type), yyjson_obj_get(v, "type"));
    copy_str(out->title, sizeof(out->title), yyjson_obj_get(v, "title"));
}

/**
 * @brief Parse a Telegram Message object.
 *
 * A message without a `from` member is treated as a service message (joins,
 * pins, and similar), which the router skips for command handling.
 *
 * @param[in]  v   JSON object (may be NULL).
 * @param[out] out Receives the parsed message (zeroed first).
 */
static void parse_message(yyjson_val *v, tg_message_t *out)
{
    memset(out, 0, sizeof(*out));
    if (v == NULL || !yyjson_is_obj(v)) {
        return;
    }
    out->message_id = get_i64(v, "message_id");
    out->date = get_i64(v, "date");
    parse_user(yyjson_obj_get(v, "from"), &out->from);
    parse_chat(yyjson_obj_get(v, "chat"), &out->chat);
    copy_str(out->text, sizeof(out->text), yyjson_obj_get(v, "text"));

    yyjson_val *reply = yyjson_obj_get(v, "reply_to_message");
    if (reply != NULL && yyjson_is_obj(reply)) {
        out->has_reply = 1;
        out->reply_to_message_id = get_i64(reply, "message_id");
        parse_user(yyjson_obj_get(reply, "from"), &out->reply_from);
    }

    yyjson_val *members = yyjson_obj_get(v, "new_chat_members");
    if (members != NULL && yyjson_is_arr(members)) {
        size_t idx = 0;
        size_t max = 0;
        yyjson_val *item = NULL;
        yyjson_arr_foreach(members, idx, max, item)
        {
            if (out->new_member_count >= TG_MAX_NEW_MEMBERS) {
                break;
            }
            parse_user(item, &out->new_members[out->new_member_count]);
            out->new_member_count++;
        }
    }

    /* A message with no sender is a service message (SPEC section 5.3). */
    out->is_service = yyjson_obj_get(v, "from") == NULL;
}

/**
 * @brief Parse a chat_member / my_chat_member object into the shared fields.
 *
 * @param[in]  obj JSON object (chat_member or my_chat_member).
 * @param[out] out Update receiving the membership fields.
 */
static void parse_membership(yyjson_val *obj, tg_update_t *out)
{
    parse_chat(yyjson_obj_get(obj, "chat"), &out->member_chat);
    parse_user(yyjson_obj_get(obj, "from"), &out->member_from);
    yyjson_val *who = yyjson_obj_get(obj, "new_chat_member");
    /* A ChatMember wraps its User under "user". */
    parse_user(who != NULL ? yyjson_obj_get(who, "user") : NULL, &out->member_user);
    yyjson_val *old = yyjson_obj_get(obj, "old_chat_member");
    copy_str(out->old_status, sizeof(out->old_status),
             old != NULL ? yyjson_obj_get(old, "status") : NULL);
    copy_str(out->new_status, sizeof(out->new_status),
             who != NULL ? yyjson_obj_get(who, "status") : NULL);
}

/**
 * @brief Parse one Update object (already extracted from its doc).
 *
 * Shared by @ref tg_update_parse and @ref tg_updates_parse so both exercise the
 * same field mapping.
 *
 * @param[in]  root Update JSON object.
 * @param[out] out  Receives the parsed update (zeroed first).
 */
static void parse_update_obj(yyjson_val *root, tg_update_t *out)
{
    memset(out, 0, sizeof(*out));
    out->update_id = get_i64(root, "update_id");

    yyjson_val *message = yyjson_obj_get(root, "message");
    yyjson_val *edited = yyjson_obj_get(root, "edited_message");
    yyjson_val *callback = yyjson_obj_get(root, "callback_query");
    yyjson_val *chat_member = yyjson_obj_get(root, "chat_member");
    yyjson_val *my_chat_member = yyjson_obj_get(root, "my_chat_member");

    if (message != NULL && yyjson_is_obj(message)) {
        out->kind = TG_UPD_MESSAGE;
        parse_message(message, &out->message);
    } else if (edited != NULL && yyjson_is_obj(edited)) {
        out->kind = TG_UPD_EDITED_MESSAGE;
        parse_message(edited, &out->message);
    } else if (callback != NULL && yyjson_is_obj(callback)) {
        out->kind = TG_UPD_CALLBACK_QUERY;
        copy_str(out->callback_id, sizeof(out->callback_id), yyjson_obj_get(callback, "id"));
        parse_user(yyjson_obj_get(callback, "from"), &out->callback_from);
        copy_str(out->callback_data, sizeof(out->callback_data), yyjson_obj_get(callback, "data"));
        yyjson_val *cmsg = yyjson_obj_get(callback, "message");
        if (cmsg != NULL && yyjson_is_obj(cmsg)) {
            out->callback_has_message = 1;
            out->callback_message_id = get_i64(cmsg, "message_id");
            parse_chat(yyjson_obj_get(cmsg, "chat"), &out->callback_chat);
        }
    } else if (chat_member != NULL && yyjson_is_obj(chat_member)) {
        out->kind = TG_UPD_CHAT_MEMBER;
        parse_membership(chat_member, out);
    } else if (my_chat_member != NULL && yyjson_is_obj(my_chat_member)) {
        out->kind = TG_UPD_MY_CHAT_MEMBER;
        parse_membership(my_chat_member, out);
    } else {
        out->kind = TG_UPD_UNKNOWN;
    }
}

int tg_update_parse(const char *json, size_t len, tg_update_t *out)
{
    if (json == NULL || out == NULL) {
        return -EINVAL;
    }
    yyjson_doc *doc = yyjson_read(json, len, 0);
    if (doc == NULL) {
        return -ERR_PARSE;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (root == NULL || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return -ERR_PARSE;
    }
    parse_update_obj(root, out);
    yyjson_doc_free(doc);
    return 0;
}

int tg_updates_parse(const char *json, size_t len, tg_update_cb cb, void *ctx, unsigned *count)
{
    if (json == NULL || cb == NULL) {
        return -EINVAL;
    }
    if (count != NULL) {
        *count = 0;
    }

    yyjson_doc *doc = yyjson_read(json, len, 0);
    if (doc == NULL) {
        return -ERR_PARSE;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *ok = root != NULL ? yyjson_obj_get(root, "ok") : NULL;
    yyjson_val *result = root != NULL ? yyjson_obj_get(root, "result") : NULL;
    if (ok == NULL || !yyjson_get_bool(ok) || result == NULL || !yyjson_is_arr(result)) {
        yyjson_doc_free(doc);
        return -ERR_PARSE;
    }

    unsigned delivered = 0;
    size_t idx = 0;
    size_t max = 0;
    yyjson_val *item = NULL;
    yyjson_arr_foreach(result, idx, max, item)
    {
        if (!yyjson_is_obj(item)) {
            continue;
        }
        tg_update_t upd;
        parse_update_obj(item, &upd);
        delivered++;
        if (cb(&upd, ctx) != 0) {
            break;
        }
    }

    yyjson_doc_free(doc);
    if (count != NULL) {
        *count = delivered;
    }
    return 0;
}
