/**
 * @file cmd_settings.c
 * @brief Settings, welcome/rules and reload handlers (SPEC sections 5.2, 5.4).
 *
 * This file holds the per-chat configuration commands:
 *   - `/setwarnlimit`, `/setwarnaction` and `/setlog` write the warn policy and
 *     the moderation log channel (SPEC section 5.5);
 *   - `/setwelcome`, `/welcome on|off`, `/setrules` and `/rules` manage the join
 *     welcome and the rules text;
 *   - `/settings` opens an inline-keyboard panel that toggles the welcome, and
 *     `/reload` refreshes the cached administrator list.
 *
 * The welcome template renderer (@ref cmd_render_welcome) is pure and unit-tested
 * for every placeholder. Writes go through @ref cmd_ctx::db_submit (the invocation
 * outlives the job) and invalidate the settings cache so the next read refetches.
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
#include "tg/cmd_ctx.h"
#include "tg/cmd_settings.h"
#include "tg/db.h"
#include "tg/tg_types.h"
#include "tg/util.h"

/* --- Pure helpers ---------------------------------------------------------- */

/** @brief A resolved welcome placeholder. */
typedef struct {
    const char *text; /**< Replacement text (points into @p scratch or a literal). */
    size_t len;       /**< Length of @p text. */
    int is_markup;    /**< 1 if @p text already carries HTML (inserted verbatim). */
} welcome_repl_t;

/**
 * @brief Resolve one placeholder at @p p, if any.
 *
 * @param[in]  p            Current template position.
 * @param[in]  first        Substituted first name.
 * @param[in]  uname        Username ("" if none).
 * @param[in]  chat_title   Chat title.
 * @param[in]  member_count Member count.
 * @param[out] scratch      Buffer backing @p out->text for dynamic values.
 * @param[in]  scratch_cap  Capacity of @p scratch.
 * @param[out] out          Receives the replacement.
 *
 * @return The number of template bytes consumed (0 if @p p is not a placeholder).
 */
static size_t welcome_resolve(const char *p, const char *first, const char *uname,
                              const char *chat_title, int member_count, char *scratch,
                              size_t scratch_cap, welcome_repl_t *out)
{
    if (strncmp(p, "{first}", 7) == 0) {
        (void)snprintf(scratch, scratch_cap, "%s", first);
        *out = (welcome_repl_t){.text = scratch, .is_markup = 0};
        return 7;
    }
    if (strncmp(p, "{mention}", 9) == 0) {
        if (uname[0] != '\0') {
            (void)snprintf(scratch, scratch_cap, "<a href=\"https://t.me/%s\">%s</a>", uname,
                           first);
            *out = (welcome_repl_t){.text = scratch, .is_markup = 1};
        } else {
            (void)snprintf(scratch, scratch_cap, "%s", first);
            *out = (welcome_repl_t){.text = scratch, .is_markup = 0};
        }
        return 9;
    }
    if (strncmp(p, "{chat}", 6) == 0) {
        (void)snprintf(scratch, scratch_cap, "%s", chat_title);
        *out = (welcome_repl_t){.text = scratch, .is_markup = 0};
        return 6;
    }
    if (strncmp(p, "{count}", 7) == 0) {
        (void)snprintf(scratch, scratch_cap, "%d", member_count);
        *out = (welcome_repl_t){.text = scratch, .is_markup = 0};
        return 7;
    }
    return 0;
}

/**
 * @brief Append @p r to @p out at @p off, escaping unless it is markup.
 *
 * @param[in]  r   Resolved replacement.
 * @param[out] out Destination buffer.
 * @param[in]  cap Capacity of @p out.
 * @param[in]  off Current write offset.
 * @param[out] new_off Receives the new offset.
 *
 * @return 0 on success, -ENOSPC if @p out is too small.
 */
static int welcome_append(const welcome_repl_t *r, char *out, size_t cap, size_t off,
                          size_t *new_off)
{
    size_t len = r->len != 0 ? r->len : strlen(r->text);
    if (r->is_markup != 0) {
        if (off + len + 1u > cap) {
            return -ENOSPC;
        }
        (void)memcpy(out + off, r->text, len);
        *new_off = off + len;
        return 0;
    }
    size_t written = 0;
    if (util_html_escape(r->text, len, out + off, cap - off, &written) != 0) {
        return -ENOSPC;
    }
    *new_off = off + written;
    return 0;
}

int cmd_render_welcome(const char *tmpl, const tg_user_t *user, const char *chat_title,
                       int member_count, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return -EINVAL;
    }
    if (tmpl == NULL) {
        tmpl = "";
    }
    if (chat_title == NULL) {
        chat_title = "";
    }
    const char *first = (user != NULL && user->first_name[0] != '\0') ? user->first_name : "there";
    const char *uname = user != NULL ? user->username : "";

    size_t off = 0;
    for (const char *p = tmpl; *p != '\0';) {
        char scratch[320];
        welcome_repl_t r = {0};
        size_t consumed = welcome_resolve(p, first, uname, chat_title, member_count, scratch,
                                          sizeof(scratch), &r);
        if (consumed == 0) {
            if (off + 1u >= cap) {
                return -ENOSPC;
            }
            out[off++] = *p++;
            continue;
        }
        p += consumed;
        if (welcome_append(&r, out, cap, off, &off) != 0) {
            return -ENOSPC;
        }
    }
    out[off] = '\0';
    return 0;
}

/* --- Settings writes ------------------------------------------------------- */

/** @brief Context for a settings write that replies with a fixed confirmation. */
typedef struct {
    cmd_call_t *call; /**< Invocation kept alive across the DB job. */
    int64_t chat_id;  /**< Chat whose cache to invalidate on success. */
    char reply[256];  /**< Success reply (plain text). */
} set_ctx_t;

/**
 * @brief Settings-write completion: invalidate the cache and reply.
 *
 * @param[in] res DB result.
 * @param[in] user The @ref set_ctx_t.
 */
static void on_set_done(const db_result_t *res, void *user)
{
    set_ctx_t *s = user;
    struct cmd_ctx *ctx = cmd_call_ctx(s->call);
    int64_t reply_to = ctx->msg != NULL ? ctx->msg->message_id : 0;
    if (res->status == 0) {
        (void)cmd_ctx_reply_plain(ctx, reply_to, s->reply);
    } else {
        (void)cmd_ctx_reply_plain(ctx, reply_to, "I could not save that setting.");
    }
    free(s);
}

/**
 * @brief Submit a settings write and reply on completion.
 *
 * @param[in] ctx    Context.
 * @param[in] mask   DB_SET_* field selector.
 * @param[in] value  Numeric value (for numeric fields).
 * @param[in] text   Text value (for text fields; may be NULL).
 * @param[in] reply  Success reply.
 *
 * @return 0 if queued, negative errno on failure.
 */
static int settings_write(struct cmd_ctx *ctx, unsigned mask, int64_t value, const char *text,
                          const char *reply)
{
    if (ctx->db_submit == NULL) {
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "Settings are unavailable right now.");
        return 0;
    }
    set_ctx_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return -ENOMEM;
    }
    s->call = ctx->call;
    s->chat_id = ctx->chat_id;
    (void)snprintf(s->reply, sizeof(s->reply), "%s", reply);
    db_job_t job = {
        .kind = DB_JOB_SETTINGS_SET, .id = ctx->chat_id, .settings_mask = mask, .value = value};
    if (text != NULL) {
        (void)snprintf(job.text, sizeof(job.text), "%s", text);
    }
    int rc = ctx->db_submit(ctx, &job, on_set_done, s);
    if (rc != 0) {
        free(s);
        return rc;
    }
    /* Drop the cached policy so the next read sees the new value. */
    if (ctx->invalidate_settings != NULL) {
        ctx->invalidate_settings(ctx);
    }
    return rc;
}

/**
 * @brief /setwarnlimit handler: set the warning count that triggers the action.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_setwarnlimit(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    char *end = NULL;
    long n = strtol(ctx->args, &end, 10);
    if (end == ctx->args || *end != '\0' || n < 0 || n > 1000) {
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "Usage: /setwarnlimit <n> (0 disables auto-action).");
        return 0;
    }
    char reply[64];
    (void)snprintf(reply, sizeof(reply), "Warn limit set to %ld.", n);
    return settings_write(ctx, DB_SET_WARN_LIMIT, n, NULL, reply);
}

/**
 * @brief /setwarnaction handler: set the action taken at the warning limit.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_setwarnaction(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    const char *a = ctx->args;
    if (strcmp(a, "ban") != 0 && strcmp(a, "mute") != 0 && strcmp(a, "kick") != 0) {
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "Usage: /setwarnaction ban|mute|kick");
        return 0;
    }
    char reply[64];
    (void)snprintf(reply, sizeof(reply), "Warn action set to '%s'.", a);
    return settings_write(ctx, DB_SET_WARN_ACTION, 0, a, reply);
}

/**
 * @brief /setlog handler: set or clear the moderation log channel.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_setlog(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    if (strcmp(ctx->args, "off") == 0) {
        return settings_write(ctx, DB_SET_LOG_CHAT, 0, NULL, "Moderation log disabled.");
    }
    char *end = NULL;
    long long id = strtoll(ctx->args, &end, 10);
    if (end == ctx->args || *end != '\0' || id == 0) {
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "Usage: /setlog <chat_id|off>");
        return 0;
    }
    char reply[96];
    (void)snprintf(reply, sizeof(reply), "Moderation log set to %lld.", id);
    return settings_write(ctx, DB_SET_LOG_CHAT, id, NULL, reply);
}

/**
 * @brief /setwelcome handler: set the join welcome template.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_setwelcome(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    if (ctx->args == NULL || ctx->args[0] == '\0') {
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "Usage: /setwelcome <text> ({first} {mention} {chat} {count})");
        return 0;
    }
    return settings_write(ctx, DB_SET_WELCOME_TEXT, 0, ctx->args,
                          "Welcome message saved. Use /welcome on to enable it.");
}

/**
 * @brief /welcome handler: enable or disable the join welcome.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_welcome(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    if (strcmp(ctx->args, "on") == 0) {
        return settings_write(ctx, DB_SET_WELCOME_ON, 1, NULL, "Welcome enabled.");
    }
    if (strcmp(ctx->args, "off") == 0) {
        return settings_write(ctx, DB_SET_WELCOME_ON, 0, NULL, "Welcome disabled.");
    }
    (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                              "Usage: /welcome on|off");
    return 0;
}

/**
 * @brief /setrules handler: set the chat rules text.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_setrules(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    if (ctx->args == NULL || ctx->args[0] == '\0') {
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "Usage: /setrules <text>");
        return 0;
    }
    return settings_write(ctx, DB_SET_RULES_TEXT, 0, ctx->args, "Rules saved.");
}

/**
 * @brief /rules handler: show the chat rules.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_rules(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    int64_t reply_to = ctx->msg != NULL ? ctx->msg->message_id : 0;
    if (ctx->settings == NULL || ctx->settings->rules_text[0] == '\0') {
        (void)cmd_ctx_reply_plain(ctx, reply_to, "No rules have been set for this chat.");
        return 0;
    }
    (void)cmd_ctx_reply_plain(ctx, reply_to, ctx->settings->rules_text);
    return 0;
}

/**
 * @brief /reload handler: drop the cached administrator list (SPEC section 5.1).
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_reload(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    if (ctx->invalidate_admins != NULL) {
        ctx->invalidate_admins(ctx);
    }
    (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                              "Administrator list refreshed.");
    return 0;
}

/* --- /settings panel ------------------------------------------------------- */

/** @brief Prefix and layout of the settings-panel callback data. */
#define SETTINGS_CB_PREFIX "set:"

/**
 * @brief Build the inline-keyboard `reply_markup` for the settings panel.
 *
 * One row toggling the welcome, labelled with its current state.
 *
 * @param[out] out      Destination buffer.
 * @param[in]  cap      Capacity of @p out.
 * @param[in]  chat_id  Chat id embedded in the callback data.
 * @param[in]  user_id  Owner of the panel (the callback is rejected for others).
 * @param[in]  enabled  1 if the welcome is currently enabled.
 *
 * @return 0 on success, -ENOSPC if @p out is too small.
 * @thread_safety Safe (no shared state).
 */
static int build_panel_markup(char *out, size_t cap, int64_t chat_id, int64_t user_id, int enabled)
{
    const char *label = enabled != 0 ? "Welcome: ON" : "Welcome: OFF";
    int n = snprintf(out, cap,
                     "\"reply_markup\":{\"inline_keyboard\":[[{\"text\":\"%s\","
                     "\"callback_data\":\"" SETTINGS_CB_PREFIX "%lld:%lld:welcome\"}]]}",
                     label, (long long)chat_id, (long long)user_id);
    if (n < 0 || (size_t)n >= cap) {
        return -ENOSPC;
    }
    return 0;
}

/**
 * @brief /settings handler: send the inline settings panel.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_settings(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    int enabled = ctx->settings != NULL ? ctx->settings->welcome_enabled : 0;
    char markup[256];
    if (build_panel_markup(markup, sizeof(markup), ctx->chat_id, ctx->user_id, enabled) != 0) {
        return -ENOSPC;
    }
    char body[512];
    int n = snprintf(body, sizeof(body),
                     "{\"chat_id\":%lld,\"text\":\"Chat settings\",\"parse_mode\":\"HTML\",%s}",
                     (long long)ctx->chat_id, markup);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return -ENOSPC;
    }
    return cmd_ctx_request(ctx, METHOD_SEND_MESSAGE, body, (size_t)n, NULL, NULL);
}

/** @brief Parse a settings-panel callback payload (see cmd_settings.h). */
int cmd_settings_parse_callback(const char *data, int64_t *out_chat, int64_t *out_user,
                                const char **out_key)
{
    if (data == NULL || out_chat == NULL || out_user == NULL) {
        return -EINVAL;
    }
    if (strncmp(data, SETTINGS_CB_PREFIX, strlen(SETTINGS_CB_PREFIX)) != 0) {
        return -EINVAL;
    }
    const char *p = data + strlen(SETTINGS_CB_PREFIX);
    char *end = NULL;
    long long chat = strtoll(p, &end, 10);
    if (end == p || *end != ':') {
        return -EINVAL;
    }
    p = end + 1;
    long long user = strtoll(p, &end, 10);
    if (end == p || *end != ':') {
        return -EINVAL;
    }
    p = end + 1;
    if (*p == '\0') {
        return -EINVAL;
    }
    *out_chat = chat;
    *out_user = user;
    if (out_key != NULL) {
        *out_key = p;
    }
    return 0;
}
