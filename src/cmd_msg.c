/**
 * @file cmd_msg.c
 * @brief Message-management handlers: /del /pin /unpin /purge (SPEC section 5.2).
 *
 * `/del` deletes the replied-to message and the command; `/pin` pins the replied-to
 * message; `/unpin` unpins the last pinned message; `/purge` deletes every message
 * from the replied-to message up to the command, in `deleteMessages` batches of 100
 * (SPEC section 5.2). All calls go through @ref cmd_ctx_request, which keeps the
 * invocation alive until the callback runs.
 *
 * @ingroup commands
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_methods.h"
#include "commands.h"
#include "tables.h"
#include "tg/api_pool.h"
#include "tg/cmd_ctx.h"
#include "tg/util.h"

/** @brief Telegram's maximum number of ids in one deleteMessages call. */
#define DELETE_BATCH 100u

/** @brief Maximum number of messages one /purge may delete. */
#define PURGE_MAX_IDS 1000u

/**
 * @brief Reply that a command must be used as a reply to a message.
 *
 * @param[in] ctx Context.
 */
static void reply_needs_reply(struct cmd_ctx *ctx)
{
    (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                              "Reply to a message to use this command.");
}

/**
 * @brief /del handler: delete the replied-to message and the command.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_del(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    if (ctx->msg == NULL || !ctx->msg->has_reply || ctx->msg->reply_to_message_id == 0) {
        reply_needs_reply(ctx);
        return 0;
    }
    /* Delete the replied-to message and the command (SPEC section 5.2: two
     * deleteMessage calls; the command's own deletion does not depend on the
     * reply, so both are issued immediately). */
    char body[128];
    (void)snprintf(body, sizeof(body), "{\"chat_id\":%lld,\"message_id\":%lld}",
                   (long long)ctx->chat_id, (long long)ctx->msg->reply_to_message_id);
    int rc = cmd_ctx_request(ctx, METHOD_DELETE_MESSAGE, body, strlen(body), NULL, NULL);
    if (rc != 0) {
        return rc;
    }
    (void)snprintf(body, sizeof(body), "{\"chat_id\":%lld,\"message_id\":%lld}",
                   (long long)ctx->chat_id, (long long)ctx->msg->message_id);
    if (ctx->record_action != NULL) {
        ctx->record_action(ctx, "del", ctx->msg->reply_from.id, "", 0);
    }
    return cmd_ctx_request(ctx, METHOD_DELETE_MESSAGE, body, strlen(body), NULL, NULL);
}

/**
 * @brief /pin handler: pin the replied-to message silently.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_pin(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    if (ctx->msg == NULL || !ctx->msg->has_reply || ctx->msg->reply_to_message_id == 0) {
        reply_needs_reply(ctx);
        return 0;
    }
    char body[160];
    (void)snprintf(body, sizeof(body),
                   "{\"chat_id\":%lld,\"message_id\":%lld,\"disable_notification\":true}",
                   (long long)ctx->chat_id, (long long)ctx->msg->reply_to_message_id);
    return cmd_ctx_request(ctx, METHOD_PIN_CHAT_MESSAGE, body, strlen(body), NULL, NULL);
}

/**
 * @brief /unpin handler: unpin the most recent pinned message.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_unpin(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    char body[96];
    (void)snprintf(body, sizeof(body), "{\"chat_id\":%lld}", (long long)ctx->chat_id);
    return cmd_ctx_request(ctx, METHOD_UNPIN_CHAT_MESSAGE, body, strlen(body), NULL, NULL);
}

/* --- /purge ---------------------------------------------------------------- */

/** @brief Context for the /purge deleteMessages batches. */
typedef struct {
    int failed; /**< 1 once any batch failed. */
} purge_ctx_t;

/**
 * @brief Completion for one /purge batch.
 *
 * @param[in] res Batch result.
 * @param[in] ctx The @ref purge_ctx_t.
 */
static void on_purge_batch(const api_result_t *res, void *ctx)
{
    purge_ctx_t *p = ctx;
    if (res->status != 0) {
        p->failed = 1;
        util_log(LOG_WARN, "cmd_msg", "purge batch failed: %d", res->status);
    }
    free(p);
}

/**
 * @brief Issue one `deleteMessages` batch for /purge.
 *
 * @param[in] ctx     Context.
 * @param[in] ids     Ids to delete.
 * @param[in] count   Number of ids.
 *
 * @return 0 if queued, negative errno on failure.
 */
static int purge_batch(struct cmd_ctx *ctx, const int64_t *ids, size_t count)
{
    char body[PURGE_MAX_IDS * 24u];
    int n = snprintf(body, sizeof(body), "{\"chat_id\":%lld,\"message_ids\":[",
                     (long long)ctx->chat_id);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return -ENOSPC;
    }
    size_t off = (size_t)n;
    for (size_t i = 0; i < count; ++i) {
        int m = snprintf(body + off, sizeof(body) - off, "%s%lld", i == 0 ? "" : ",",
                         (long long)ids[i]);
        if (m < 0 || (size_t)m >= sizeof(body) - off) {
            return -ENOSPC;
        }
        off += (size_t)m;
    }
    if (off + 2u >= sizeof(body)) {
        return -ENOSPC;
    }
    body[off++] = ']';
    body[off++] = '}';
    body[off] = '\0';

    purge_ctx_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return -ENOMEM;
    }
    int rc = cmd_ctx_request(ctx, METHOD_DELETE_MESSAGES, body, off, on_purge_batch, p);
    if (rc != 0) {
        free(p);
    }
    return rc;
}

/**
 * @brief Collect the message ids a /purge should delete, oldest first.
 *
 * Every recent id in `[reply_to_message_id, message_id]` is included, and the
 * replied-to message and the command are appended if the ring did not contain
 * them, so /purge always removes its own trigger.
 *
 * @param[in]  ctx   Context.
 * @param[out] ids   Destination array (capacity @ref PURGE_MAX_IDS).
 * @param[in]  cap   Capacity of @p ids.
 *
 * @return The number of ids collected.
 */
static size_t purge_collect_ids(const struct cmd_ctx *ctx, int64_t *ids, size_t cap)
{
    int64_t lo = ctx->msg->reply_to_message_id;
    int64_t hi = ctx->msg->message_id;
    size_t count = 0;
    int have_lo = 0;
    int have_hi = 0;
    for (size_t i = 0; i < ctx->recent_count && count < cap; ++i) {
        int64_t id = ctx->recent_ids[i];
        if (id < lo || id > hi) {
            continue;
        }
        ids[count++] = id;
        have_lo = have_lo || id == lo;
        have_hi = have_hi || id == hi;
    }
    if (!have_lo && count < cap) {
        ids[count++] = lo;
    }
    if (!have_hi && count < cap) {
        ids[count++] = hi;
    }
    return count;
}

/**
 * @brief /purge handler: delete messages from the reply to the command.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_purge(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    if (ctx->msg == NULL || !ctx->msg->has_reply || ctx->msg->reply_to_message_id == 0) {
        reply_needs_reply(ctx);
        return 0;
    }

    /* The router supplies the per-chat recent-id ring; the command id itself is
     * always included so /purge also removes its own trigger. */
    int64_t ids[PURGE_MAX_IDS];
    size_t count = purge_collect_ids(ctx, ids, PURGE_MAX_IDS);

    if (ctx->record_action != NULL) {
        ctx->record_action(ctx, "purge", 0, "", 0);
    }

    int rc = 0;
    for (size_t off = 0; off < count && rc == 0; off += DELETE_BATCH) {
        size_t batch = count - off;
        if (batch > DELETE_BATCH) {
            batch = DELETE_BATCH;
        }
        rc = purge_batch(ctx, ids + off, batch);
    }
    return rc;
}
