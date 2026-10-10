/**
 * @file perms.c
 * @brief Permission model and per-chat administrator cache (SPEC section 5.1).
 *
 * @ingroup perms
 */
#include "tg/perms.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "api_methods.h"
#include "commands.h"
#include "tables.h"
#include "tg/api_pool.h"
#include "tg/cache.h"
#include "tg/errors.h"
#include "tg/util.h"

/** @brief A pending getChatAdministrators request awaiting its reply. */
typedef struct {
    perms_t *perms;     /**< Engine to cache into. */
    int64_t chat_id;    /**< Chat being fetched. */
    perms_admins_cb cb; /**< Caller callback. */
    void *ctx;          /**< Caller context. */
} perms_fetch_ctx_t;

/** @brief The permission engine. */
struct perms {
    perms_config_t cfg; /**< Copied configuration. */
    cache_t *cache;     /**< chat_id -> perms_admins_t. */
};

/* --- Pure decision logic --------------------------------------------------- */

int perms_check(const perms_subject_t *subject, const cmd_desc_t *cmd, int bot_has_right)
{
    if (subject == NULL) {
        return -EINVAL;
    }
    if (cmd == NULL) {
        return 0;
    }

    /* The bot must hold its required right in the chat regardless of the caller. */
    if (cmd->bot_right != RIGHT_NONE && !bot_has_right) {
        return -ERR_TG_API;
    }

    /* Sudo and the chat owner bypass the caller-right check. */
    if (subject->is_sudo || subject->role == ROLE_OWNER) {
        return 0;
    }

    if (cmd->min_role == ROLE_OWNER) {
        return -ERR_PERM;
    }
    if (cmd->min_role == ROLE_SUDO && !subject->is_sudo) {
        return -ERR_PERM;
    }
    if (cmd->min_role == ROLE_ADMIN && subject->role != ROLE_ADMIN) {
        return -ERR_PERM;
    }

    /* An administrator must additionally hold the command's caller right. */
    if (cmd->user_right != RIGHT_NONE && subject->role == ROLE_ADMIN &&
        (subject->rights & RIGHT_BIT(cmd->user_right)) == 0u) {
        return -ERR_PERM;
    }
    return 0;
}

int perms_bot_has_right(const perms_admins_t *admins, int64_t bot_id, right_t right)
{
    if (right == RIGHT_NONE) {
        return 1;
    }
    if (admins == NULL) {
        return 0;
    }
    for (unsigned i = 0; i < admins->count; ++i) {
        if (admins->admins[i].user_id != bot_id) {
            continue;
        }
        if (admins->admins[i].role == ROLE_OWNER) {
            return 1; /* the creator holds every right */
        }
        return (admins->admins[i].rights & RIGHT_BIT(right)) != 0u;
    }
    return 0; /* the bot is not an administrator at all */
}

role_t perms_role_of(const perms_admins_t *admins, int64_t user_id)
{
    if (admins == NULL) {
        return ROLE_ANYONE;
    }
    for (unsigned i = 0; i < admins->count; ++i) {
        if (admins->admins[i].user_id == user_id) {
            return admins->admins[i].role;
        }
    }
    return ROLE_ANYONE;
}

int perms_target_ok(role_t target, int64_t target_id, int64_t bot_id)
{
    if (target_id == bot_id) {
        return -ERR_PERM; /* never act on the bot itself */
    }
    if (target == ROLE_OWNER || target == ROLE_ADMIN) {
        return -ERR_PERM; /* never act on the owner or another administrator */
    }
    return 0;
}

/* --- Admin list parsing ---------------------------------------------------- */

/**
 * @brief Map a ChatMember `status`/rights object to a role and right bitmask.
 *
 * @param[in]  member JSON ChatMember object.
 * @param[out] role   Receives ROLE_OWNER or ROLE_ADMIN.
 *
 * @return Bitmask of held rights.
 */
static unsigned parse_member_rights(yyjson_val *member, role_t *role)
{
    *role = ROLE_ADMIN;
    yyjson_val *status = yyjson_obj_get(member, "status");
    if (status != NULL && yyjson_is_str(status) && strcmp(yyjson_get_str(status), "creator") == 0) {
        *role = ROLE_OWNER;
        return 0xffffffffu; /* the creator holds every right */
    }

    unsigned rights = 0;
    static const struct {
        const char *field;
        right_t right;
    } map[] = {
        {"can_restrict_members", RIGHT_RESTRICT}, {"can_delete_messages", RIGHT_DELETE},
        {"can_pin_messages", RIGHT_PIN},          {"can_promote_members", RIGHT_PROMOTE},
        {"can_change_info", RIGHT_CHANGE_INFO},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        yyjson_val *v = yyjson_obj_get(member, map[i].field);
        if (v != NULL && yyjson_get_bool(v)) {
            rights |= RIGHT_BIT(map[i].right);
        }
    }
    return rights;
}

/**
 * @brief Copy a JSON string member of @p obj into a fixed buffer.
 *
 * @param[out] dst Destination buffer.
 * @param[in]  cap Capacity of @p dst.
 * @param[in]  obj JSON object.
 * @param[in]  key Member name.
 */
static void copy_member_str(char *dst, size_t cap, yyjson_val *obj, const char *key)
{
    yyjson_val *v = yyjson_obj_get(obj, key);
    if (v != NULL && yyjson_is_str(v)) {
        (void)snprintf(dst, cap, "%s", yyjson_get_str(v));
    }
}

/**
 * @brief Parse one ChatMember entry into @p a.
 *
 * @param[in]  item JSON ChatMember object.
 * @param[out] a    Receives the parsed administrator.
 *
 * @return 0 if @p item yielded an administrator, -EINVAL if it was malformed.
 */
static int parse_admin_entry(yyjson_val *item, perms_admin_t *a)
{
    yyjson_val *user = yyjson_obj_get(item, "user");
    if (user == NULL || !yyjson_is_obj(user)) {
        return -EINVAL;
    }
    yyjson_val *id = yyjson_obj_get(user, "id");
    if (id == NULL || !yyjson_is_int(id)) {
        return -EINVAL;
    }
    memset(a, 0, sizeof(*a));
    a->user_id = (int64_t)yyjson_get_sint(id);
    a->rights = parse_member_rights(item, &a->role);
    copy_member_str(a->username, sizeof(a->username), user, "username");
    copy_member_str(a->first_name, sizeof(a->first_name), user, "first_name");
    return 0;
}

/**
 * @brief Parse a getChatAdministrators result array into @p out.
 *
 * @param[in]  json Reply body.
 * @param[in]  len  Length.
 * @param[out] out  Receives the parsed administrators (zeroed first).
 *
 * @return 0 on success, -ERR_PARSE on malformed input.
 */
static int parse_admins(const char *json, size_t len, perms_admins_t *out)
{
    memset(out, 0, sizeof(*out));
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

    size_t idx = 0;
    size_t max = 0;
    yyjson_val *item = NULL;
    yyjson_arr_foreach(result, idx, max, item)
    {
        if (out->count >= PERMS_MAX_ADMINS) {
            break;
        }
        if (parse_admin_entry(item, &out->admins[out->count]) == 0) {
            out->count++;
        }
    }
    yyjson_doc_free(doc);
    return 0;
}

/* --- Admin cache ----------------------------------------------------------- */

/**
 * @brief getChatAdministrators completion that caches and forwards the result.
 *
 * @param[in] res API result.
 * @param[in] ctx The @ref perms_fetch_ctx_t.
 */
static void on_admins_fetch(const api_result_t *res, void *ctx)
{
    perms_fetch_ctx_t *f = ctx;
    perms_admins_t admins;
    int status = res->status;
    if (status == 0) {
        status = parse_admins(res->body, res->body_len, &admins);
    }
    if (status == 0) {
        admins.chat_id = f->chat_id;
        char key[32];
        (void)snprintf(key, sizeof(key), "%lld", (long long)f->chat_id);
        if (cache_put(f->perms->cache, key, &admins, sizeof(admins), PERMS_TTL_MS) != 0) {
            util_log(LOG_WARN, "perms", "failed to cache admins for %lld", (long long)f->chat_id);
        }
        f->cb(&admins, 0, f->ctx);
    } else {
        f->cb(NULL, status, f->ctx);
    }
    free(f);
}

int perms_admins_get(perms_t *p, int64_t chat_id, perms_admins_cb cb, void *ctx)
{
    if (p == NULL || cb == NULL) {
        return -EINVAL;
    }

    char key[32];
    (void)snprintf(key, sizeof(key), "%lld", (long long)chat_id);
    size_t len = 0;
    const perms_admins_t *cached = cache_get(p->cache, key, &len);
    if (cached != NULL && len == sizeof(*cached)) {
        cb(cached, 0, ctx); /* synchronous hit */
        return 0;
    }

    perms_fetch_ctx_t *f = calloc(1, sizeof(*f));
    if (f == NULL) {
        return -ENOMEM;
    }
    f->perms = p;
    f->chat_id = chat_id;
    f->cb = cb;
    f->ctx = ctx;

    char body[64];
    (void)snprintf(body, sizeof(body), "{\"chat_id\":%lld}", (long long)chat_id);
    int rc = p->cfg.api->request(p->cfg.api->ctx, METHOD_GET_CHAT_ADMINISTRATORS, body,
                                 strlen(body), chat_id, 0, on_admins_fetch, f);
    if (rc != 0) {
        free(f);
        return rc;
    }
    return 0;
}

int perms_subject_from_admins(const perms_t *p, const perms_admins_t *admins, int64_t user_id,
                              perms_subject_t *out)
{
    if (out == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->role = ROLE_ANYONE; /* "member" is not a role value; treated as non-admin */
    out->is_sudo = perms_is_sudo(p, user_id);

    if (admins != NULL) {
        for (unsigned i = 0; i < admins->count; ++i) {
            if (admins->admins[i].user_id == user_id) {
                out->role = admins->admins[i].role;
                out->rights = admins->admins[i].rights;
                break;
            }
        }
    }
    return 0;
}

/** @brief Context for perms_subject_get's admin-fetch callback. */
typedef struct {
    perms_t *perms;      /**< Engine. */
    int64_t user_id;     /**< User to classify. */
    perms_subject_cb cb; /**< Caller callback. */
    void *ctx;           /**< Caller context. */
} perms_subject_ctx_t;

/**
 * @brief Admin-fetch completion for @ref perms_subject_get.
 *
 * @param[in] admins Administrator list (NULL on failure).
 * @param[in] status Fetch status.
 * @param[in] ctx    The @ref perms_subject_ctx_t.
 */
static void on_subject_admins(const perms_admins_t *admins, int status, void *ctx)
{
    perms_subject_ctx_t *s = ctx;
    if (status == 0) {
        perms_subject_t subject;
        (void)perms_subject_from_admins(s->perms, admins, s->user_id, &subject);
        s->cb(&subject, 0, s->ctx);
    } else {
        s->cb(NULL, status, s->ctx);
    }
    free(s);
}

int perms_subject_get(perms_t *p, int64_t chat_id, int64_t user_id, perms_subject_cb cb, void *ctx)
{
    if (p == NULL || cb == NULL) {
        return -EINVAL;
    }
    perms_subject_ctx_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return -ENOMEM;
    }
    s->perms = p;
    s->user_id = user_id;
    s->cb = cb;
    s->ctx = ctx;
    int rc = perms_admins_get(p, chat_id, on_subject_admins, s);
    if (rc != 0) {
        free(s);
        return rc;
    }
    return 0;
}

int perms_is_sudo(const perms_t *p, int64_t user_id)
{
    if (p == NULL) {
        return 0;
    }
    for (size_t i = 0; i < p->cfg.sudo_count; ++i) {
        if (p->cfg.sudo_ids[i] == user_id) {
            return 1;
        }
    }
    return 0;
}

void perms_invalidate(perms_t *p, int64_t chat_id)
{
    if (p == NULL) {
        return;
    }
    char key[32];
    (void)snprintf(key, sizeof(key), "%lld", (long long)chat_id);
    (void)cache_remove(p->cache, key);
}

/* --- Lifecycle ------------------------------------------------------------- */

perms_t *perms_create(const perms_config_t *cfg)
{
    if (cfg == NULL || cfg->clock == NULL || cfg->api == NULL || cfg->api->request == NULL) {
        return NULL;
    }
    perms_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->cfg = *cfg;
    p->cache = cache_create(cfg->clock, 256);
    if (p->cache == NULL) {
        free(p);
        return NULL;
    }
    return p;
}

void perms_destroy(perms_t *p)
{
    if (p == NULL) {
        return;
    }
    cache_destroy(p->cache);
    free(p);
}
