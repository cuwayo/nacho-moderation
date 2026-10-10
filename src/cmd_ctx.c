/**
 * @file cmd_ctx.c
 * @brief Reference-counted command invocation, reply and action-recording helpers.
 *
 * See cmd_ctx.h for the design. The invocation is a small heap object with a
 * reference count: the router holds one reference while the handler runs, and
 * every in-flight API call issued through @c cmd_ctx_request holds another. A
 * handler that starts an asynchronous chain (e.g. `/kick`) may therefore return
 * before the chain finishes; the invocation outlives it and is freed by the last
 * callback that releases it (SPEC section 9).
 *
 * @ingroup commands
 */
#include "tg/cmd_ctx.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_methods.h"
#include "commands.h"
#include "tables.h"
#include "tg/api_pool.h"
#include "tg/db.h"
#include "tg/tg_api.h"
#include "tg/tg_types.h"
#include "tg/util.h"

/** @brief The reference-counted invocation. */
struct cmd_call {
    int refs;                                /**< Live references. */
    struct cmd_ctx ctx;                      /**< The context handed to the handler. */
    tg_message_t msg;                        /**< Copy of the originating message (async-safe). */
    const tg_api_ops_t *api;                 /**< API seam (borrowed; outlives the call). */
    const db_ops_t *db;                      /**< DB seam (borrowed; may be NULL). */
    const wall_clock_ops_t *wall;            /**< Wall clock (borrowed). */
    int64_t *recent;                         /**< Owned ring of recent message ids (for /purge). */
    size_t recent_count;                     /**< Entries in @ref recent. */
    char target_label[CMD_TARGET_LABEL_MAX]; /**< Storage for @c ctx.target_label. */
};

/* --- JSON body helpers ----------------------------------------------------- */

/**
 * @brief Append the JSON escape for @p c, or NULL when it needs no escaping.
 *
 * @param[in] c Byte to escape.
 *
 * @return The escape sequence, or NULL.
 */
static const char *json_escape(char c)
{
    switch (c) {
    case '"':
        return "\\\"";
    case '\\':
        return "\\\\";
    case '\n':
        return "\\n";
    case '\r':
        return "\\r";
    case '\t':
        return "\\t";
    default:
        return NULL;
    }
}

/**
 * @brief Append @p s to @p body at @p off, JSON-escaping as needed.
 *
 * @param[out] body Destination buffer.
 * @param[in]  cap  Capacity of @p body.
 * @param[in]  off  Current write offset.
 * @param[in]  s    String to append.
 *
 * @return The new offset, or (size_t)-1 if @p body is too small.
 */
static size_t json_append(char *body, size_t cap, size_t off, const char *s)
{
    for (; *s != '\0'; ++s) {
        const char *esc = json_escape(*s);
        size_t need = esc != NULL ? strlen(esc) : 1u;
        if (off + need + 1u >= cap) {
            return (size_t)-1;
        }
        if (esc != NULL) {
            (void)memcpy(body + off, esc, need);
        } else {
            body[off] = *s;
        }
        off += need;
    }
    body[off] = '\0';
    return off;
}

/**
 * @brief Build a `sendMessage` body carrying @p html as an HTML reply.
 *
 * @param[out] body     Destination buffer.
 * @param[in]  cap      Capacity of @p body.
 * @param[in]  chat_id  Target chat.
 * @param[in]  html     HTML text.
 * @param[in]  reply_to Message id to reply to (0 for none).
 * @param[out] outlen   Receives the body length.
 *
 * @return 0 on success, -ENOSPC if @p body is too small.
 */
static int build_send_message(char *body, size_t cap, int64_t chat_id, const char *html,
                              int64_t reply_to, size_t *outlen)
{
    int n = snprintf(body, cap, "{\"chat_id\":%lld,\"text\":\"", (long long)chat_id);
    if (n < 0 || (size_t)n >= cap) {
        return -ENOSPC;
    }
    size_t off = json_append(body, cap, (size_t)n, html);
    if (off == (size_t)-1) {
        return -ENOSPC;
    }
    if (reply_to != 0) {
        n = snprintf(body + off, cap - off,
                     "\",\"parse_mode\":\"HTML\",\"reply_to_message_id\":%lld}",
                     (long long)reply_to);
    } else {
        n = snprintf(body + off, cap - off, "\",\"parse_mode\":\"HTML\"}");
    }
    if (n < 0 || (size_t)n >= cap - off) {
        return -ENOSPC;
    }
    *outlen = off + (size_t)n;
    return 0;
}

/* --- Reply / action hooks -------------------------------------------------- */

/**
 * @brief The @c cmd_ctx.reply implementation: send an HTML message via the API seam.
 *
 * @param[in] ctx      The context.
 * @param[in] html     HTML text (sent verbatim).
 * @param[in] reply_to Message id to reply to (0 for none).
 *
 * @return 0 if queued, negative errno on failure.
 */
static int call_reply(struct cmd_ctx *ctx, const char *html, int64_t reply_to)
{
    char body[8192];
    size_t len = 0;
    int rc = build_send_message(body, sizeof(body), ctx->chat_id, html, reply_to, &len);
    if (rc != 0) {
        return rc;
    }
    return cmd_ctx_request(ctx, METHOD_SEND_MESSAGE, body, len, NULL, NULL);
}

/**
 * @brief No-op completion for the fire-and-forget `actions` insert.
 *
 * @param[in] res DB result (ignored).
 * @param[in] user Unused.
 */
static void on_action_recorded(const db_result_t *res, void *user)
{
    (void)user;
    if (res->status != 0) {
        util_log(LOG_WARN, "cmd_ctx", "failed to record action: %d", res->status);
    }
}

/**
 * @brief The @c cmd_ctx.record_action implementation: insert an `actions` row.
 *
 * @param[in] ctx        The context.
 * @param[in] action     Action name.
 * @param[in] target_id  Target user id.
 * @param[in] reason     Reason (may be "").
 * @param[in] expires_at Expiry (Unix seconds; 0 = permanent).
 */
static void call_record_action(struct cmd_ctx *ctx, const char *action, int64_t target_id,
                               const char *reason, int64_t expires_at)
{
    cmd_call_t *call = ctx->call;
    if (call->db == NULL || call->db->submit == NULL) {
        return;
    }
    db_job_t job = {.kind = DB_JOB_INSERT_ACTION,
                    .id = ctx->chat_id,
                    .user_id = target_id,
                    .admin_id = ctx->user_id,
                    .created_at = cmd_ctx_now(ctx),
                    .expires_at = expires_at};
    (void)snprintf(job.action, sizeof(job.action), "%s", action != NULL ? action : "");
    (void)snprintf(job.reason, sizeof(job.reason), "%s", reason != NULL ? reason : "");
    (void)call->db->submit(call->db->ctx, &job, on_action_recorded, NULL);
}

/* --- Lifecycle ------------------------------------------------------------- */

cmd_call_t *cmd_call_create(const tg_api_ops_t *api, const db_ops_t *db,
                            const wall_clock_ops_t *wall)
{
    cmd_call_t *call = calloc(1, sizeof(*call));
    if (call == NULL) {
        return NULL;
    }
    call->refs = 1;
    call->api = api;
    call->db = db;
    call->wall = wall;
    call->ctx.call = call;
    call->ctx.msg = &call->msg;
    call->ctx.args = "";
    call->ctx.target_label = call->target_label;
    call->ctx.wall = wall;
    call->ctx.reply = call_reply;
    call->ctx.reply_ctx = call;
    call->ctx.record_action = call_record_action;
    call->ctx.record_action_ctx = call;
    return call;
}

struct cmd_ctx *cmd_call_ctx(cmd_call_t *call)
{
    return call == NULL ? NULL : &call->ctx;
}

tg_message_t *cmd_call_message(cmd_call_t *call)
{
    return call == NULL ? NULL : &call->msg;
}

int cmd_call_set_target_label(cmd_call_t *call, const char *label)
{
    if (call == NULL) {
        return -EINVAL;
    }
    (void)snprintf(call->target_label, sizeof(call->target_label), "%s",
                   label != NULL ? label : "");
    call->ctx.target_label = call->target_label;
    return 0;
}

int cmd_call_set_recent(cmd_call_t *call, const int64_t *ids, size_t count)
{
    if (call == NULL || (ids == NULL && count != 0)) {
        return -EINVAL;
    }
    free(call->recent);
    call->recent = NULL;
    call->recent_count = 0;
    call->ctx.recent_ids = NULL;
    call->ctx.recent_count = 0;
    if (count == 0) {
        return 0;
    }
    if (count > CMD_RECENT_MAX) {
        ids += count - CMD_RECENT_MAX; /* keep the newest ids */
        count = CMD_RECENT_MAX;
    }
    int64_t *copy = malloc(count * sizeof(*copy));
    if (copy == NULL) {
        return -ENOMEM;
    }
    (void)memcpy(copy, ids, count * sizeof(*copy));
    call->recent = copy;
    call->recent_count = count;
    call->ctx.recent_ids = copy;
    call->ctx.recent_count = count;
    return 0;
}

void cmd_call_retain(cmd_call_t *call)
{
    if (call != NULL) {
        call->refs++;
    }
}

void cmd_call_release(cmd_call_t *call)
{
    if (call == NULL) {
        return;
    }
    if (--call->refs > 0) {
        return;
    }
    free(call->recent);
    free(call);
}

/* --- Request wrapper ------------------------------------------------------- */

/** @brief Context for the retaining callback wrapper. */
typedef struct {
    cmd_call_t *call; /**< Call kept alive across the request. */
    tg_api_cb cb;     /**< User callback. */
    void *user;       /**< User context. */
} retain_ctx_t;

/**
 * @brief Wrapper completion: run the user callback, then drop the call reference.
 *
 * @param[in] res API result.
 * @param[in] ctx The @ref retain_ctx_t.
 */
static void on_retained(const api_result_t *res, void *ctx)
{
    retain_ctx_t *rc = ctx;
    if (rc->cb != NULL) {
        rc->cb(res, rc->user);
    }
    cmd_call_release(rc->call);
    free(rc);
}

int cmd_ctx_request(struct cmd_ctx *ctx, method_id_t method, const char *body, size_t len,
                    tg_api_cb cb, void *user)
{
    if (ctx == NULL || ctx->call == NULL || ctx->call->api == NULL ||
        ctx->call->api->request == NULL) {
        return -EINVAL;
    }
    cmd_call_t *call = ctx->call;

    /* Always wrap: the invocation must outlive every in-flight request, including
     * fire-and-forget ones (the API seam requires a non-NULL callback). */
    retain_ctx_t *rc = calloc(1, sizeof(*rc));
    if (rc == NULL) {
        return -ENOMEM;
    }
    rc->call = call;
    rc->cb = cb;
    rc->user = user;
    cmd_call_retain(call);
    int rc_ret =
        call->api->request(call->api->ctx, method, body, len, ctx->chat_id, 0, on_retained, rc);
    if (rc_ret != 0) {
        cmd_call_release(call); /* undo the retain: the callback will not run */
        free(rc);
    }
    return rc_ret;
}

/* --- Reply helpers --------------------------------------------------------- */

int cmd_ctx_reply_plain(struct cmd_ctx *ctx, int64_t reply_to, const char *text)
{
    if (ctx == NULL || ctx->reply == NULL || text == NULL) {
        return -EINVAL;
    }
    size_t inlen = strlen(text);
    /* Worst case every byte becomes a 6-byte entity, plus the NUL. */
    size_t cap = inlen * 6u + 1u;
    char *esc = malloc(cap);
    if (esc == NULL) {
        return -ENOMEM;
    }
    size_t outlen = 0;
    int rc = util_html_escape(text, inlen, esc, cap, &outlen);
    if (rc == 0) {
        rc = ctx->reply(ctx, esc, reply_to);
    }
    free(esc);
    return rc;
}

/**
 * @brief Format then send an HTML reply, escaping the result (see cmd_ctx.h).
 *
 * @param[in] ctx      Context.
 * @param[in] reply_to Message id to reply to (0 for none).
 * @param[in] fmt      printf format producing plain text.
 * @param[in] ...      Format arguments.
 *
 * @return 0 if queued, negative errno on failure.
 */
int cmd_ctx_replyf(struct cmd_ctx *ctx, int64_t reply_to, const char *fmt, ...)
{
    if (ctx == NULL || fmt == NULL) {
        return -EINVAL;
    }
    va_list ap;
    va_start(ap, fmt);
    char stack[1024];
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        return -EINVAL;
    }
    int rc;
    if ((size_t)need < sizeof(stack)) {
        rc = cmd_ctx_reply_plain(ctx, reply_to, stack);
        va_end(ap2);
        return rc;
    }
    char *heap = malloc((size_t)need + 1u);
    if (heap == NULL) {
        va_end(ap2);
        return -ENOMEM;
    }
    (void)vsnprintf(heap, (size_t)need + 1u, fmt, ap2);
    va_end(ap2);
    rc = cmd_ctx_reply_plain(ctx, reply_to, heap);
    free(heap);
    return rc;
}

int64_t cmd_ctx_now(const struct cmd_ctx *ctx)
{
    if (ctx == NULL || ctx->wall == NULL) {
        return 0;
    }
    return wall_now_unix(ctx->wall);
}
