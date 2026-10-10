/**
 * @file cmd_automod.c
 * @brief Automod configuration handlers: /lock /unlock /blacklist /antiflood
 *        /captcha (SPEC section 5.2).
 *
 * These commands write the per-chat automod policy: the lock bitmask, the
 * blacklist table, the antiflood threshold and the captcha toggle. Writes go
 * through @ref cmd_ctx::db_submit (the invocation outlives the job) and
 * invalidate the settings cache so the next read refetches; the lock-type and
 * argument parsing is pure and unit-tested.
 *
 * @ingroup automod
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "tg/cmd_ctx.h"
#include "tg/db.h"
#include "tg/util.h"

/**
 * @brief Look up a lock type by name (pure).
 *
 * Returns the single-bit mask from the `bit` column of `src/locks.def` (SSOT
 * S4), so the command and the automod classifier cannot drift.
 *
 * @param[in] name Lock argument (e.g. "links").
 *
 * @return The lock bit mask, or 0 if @p name is not a lock type.
 * @thread_safety Safe (no shared state).
 */
static unsigned cmd_lock_bit(const char *name)
{
    if (name == NULL) {
        return 0;
    }
    /*! @cond */ /* X-macro expansion over locks.def (see SSOT S4); not a real macro. */
#define L(id, lname, bit, brief)                                                                   \
    if (strcmp(name, lname) == 0) {                                                                \
        return (unsigned)(1u << (bit));                                                            \
    }
#include "locks.def"
#undef L
    /*! @endcond */
    return 0;
}

/**
 * @brief Split @p args on spaces and commas, invoking @p fn per token (pure).
 *
 * @param[in] args Command tail.
 * @param[in] fn   Visitor returning 0 to continue, non-zero to stop.
 * @param[in] ctx  Visitor context.
 *
 * @return The first non-zero visitor return, or 0 if all tokens were visited.
 * @thread_safety Safe (no shared state).
 */
static int foreach_token(const char *args, int (*fn)(const char *tok, void *ctx), void *ctx)
{
    if (args == NULL) {
        return 0;
    }
    const char *p = args;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }
        const char *start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != ',') {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (len == 0) {
            continue;
        }
        char tok[64];
        if (len >= sizeof(tok)) {
            len = sizeof(tok) - 1u;
        }
        (void)memcpy(tok, start, len);
        tok[len] = '\0';
        int rc = fn(tok, ctx);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* --- Settings write plumbing ---------------------------------------------- */

/** @brief Context for a settings write that replies with a fixed confirmation. */
typedef struct {
    cmd_call_t *call; /**< Invocation kept alive across the DB job. */
    char reply[256];  /**< Success reply (plain text). */
} automod_set_ctx_t;

/**
 * @brief Settings-write completion: reply with success or failure.
 *
 * @param[in] res  DB result.
 * @param[in] user The @ref automod_set_ctx_t (freed here).
 */
static void on_automod_set(const db_result_t *res, void *user)
{
    automod_set_ctx_t *s = user;
    struct cmd_ctx *ctx = cmd_call_ctx(s->call);
    int64_t reply_to = ctx->msg != NULL ? ctx->msg->message_id : 0;
    (void)cmd_ctx_reply_plain(ctx, reply_to,
                              res->status == 0 ? s->reply : "I could not save that setting.");
    free(s);
}

/**
 * @brief Submit a masked settings write and reply on completion.
 *
 * @param[in] ctx   Context.
 * @param[in] mask  DB_SET_* selector.
 * @param[in] value Numeric value.
 * @param[in] reply Success reply.
 *
 * @return 0 if queued, negative errno on failure.
 */
static int automod_write(struct cmd_ctx *ctx, unsigned mask, int64_t value, const char *reply)
{
    if (ctx->db_submit == NULL) {
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "Settings are unavailable right now.");
        return 0;
    }
    automod_set_ctx_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return -ENOMEM;
    }
    s->call = ctx->call;
    (void)snprintf(s->reply, sizeof(s->reply), "%s", reply);
    db_job_t job = {
        .kind = DB_JOB_SETTINGS_SET, .id = ctx->chat_id, .settings_mask = mask, .value = value};
    int rc = ctx->db_submit(ctx, &job, on_automod_set, s);
    if (rc != 0) {
        free(s);
        return rc;
    }
    if (ctx->invalidate_settings != NULL) {
        ctx->invalidate_settings(ctx);
    }
    return rc;
}

/* --- /lock and /unlock ----------------------------------------------------- */

/** @brief Accumulator for the /lock and /unlock token visitor. */
typedef struct {
    unsigned found; /**< Bits recognized so far. */
    int has_bad;    /**< 1 if an unrecognized token was seen. */
    char bad[64];   /**< Copy of the first unrecognized token. */
} lock_parse_t;

/**
 * @brief Visitor: map one token to a lock bit.
 *
 * The unrecognized token is copied into @p ctx (not referenced) because the
 * caller's token buffer is a stack local that does not outlive the visitor.
 *
 * @param[in] tok Token.
 * @param[in] ctx The @ref lock_parse_t.
 *
 * @return 0 always (keep visiting).
 */
static int lock_token(const char *tok, void *ctx)
{
    lock_parse_t *lp = ctx;
    unsigned bit = cmd_lock_bit(tok);
    if (bit == 0) {
        if (!lp->has_bad) {
            lp->has_bad = 1;
            (void)snprintf(lp->bad, sizeof(lp->bad), "%s", tok);
        }
    } else {
        lp->found |= bit;
    }
    return 0;
}

/**
 * @brief Apply a lock change (add or remove bits) and reply.
 *
 * @param[in] ctx    Context.
 * @param[in] adding 1 to lock, 0 to unlock.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
static int lock_change(struct cmd_ctx *ctx, int adding)
{
    lock_parse_t lp = {0};
    (void)foreach_token(ctx->args, lock_token, &lp);
    if (lp.has_bad) {
        (void)cmd_ctx_replyf(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                             "Unknown lock type '%s'. See /help.", lp.bad);
        return 0;
    }
    if (lp.found == 0) {
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "Usage: /lock <type> [type...] (links, media, stickers, "
                                  "forwards, polls, invites, bots, commands)");
        return 0;
    }
    unsigned current = ctx->settings != NULL ? ctx->settings->locks : 0;
    unsigned updated = adding != 0 ? (current | lp.found) : (current & ~lp.found);
    char reply[64];
    (void)snprintf(reply, sizeof(reply), adding != 0 ? "Locked." : "Unlocked.");
    return automod_write(ctx, DB_SET_LOCKS, (int64_t)updated, reply);
}

/**
 * @brief /lock handler: add lock types to the chat's lock bitmask.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_lock(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    return lock_change(ctx, 1);
}

/**
 * @brief /unlock handler: remove lock types from the chat's lock bitmask.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_unlock(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    return lock_change(ctx, 0);
}

/* --- /blacklist ------------------------------------------------------------ */

/**
 * @brief Parse a blacklist pattern: `/regex/` marks it as a regular expression.
 *
 * @param[in]  raw       Raw argument.
 * @param[out] out       Destination for the (possibly unquoted) pattern.
 * @param[in]  cap       Capacity of @p out.
 * @param[out] out_regex Receives 1 if the pattern is a regex.
 *
 * @return 0 on success, -EINVAL if @p raw is empty.
 */
static int parse_pattern(const char *raw, char *out, size_t cap, int *out_regex)
{
    *out_regex = 0;
    size_t len = strlen(raw);
    if (len == 0) {
        return -EINVAL;
    }
    if (len >= 2u && raw[0] == '/' && raw[len - 1u] == '/') {
        len -= 2u;
        raw++;
        *out_regex = 1;
    }
    if (len >= cap) {
        len = cap - 1u;
    }
    (void)memcpy(out, raw, len);
    out[len] = '\0';
    return out[0] == '\0' ? -EINVAL : 0;
}

/** @brief Context for a blacklist list query. */
typedef struct {
    cmd_call_t *call; /**< Invocation kept alive across the DB job. */
} blacklist_list_ctx_t;

/**
 * @brief Blacklist-list completion: render the patterns and reply.
 *
 * @param[in] res  DB result carrying @c blacklist / @c blacklist_count.
 * @param[in] user The @ref blacklist_list_ctx_t (freed here).
 */
static void on_blacklist_listed(const db_result_t *res, void *user)
{
    blacklist_list_ctx_t *l = user;
    struct cmd_ctx *ctx = cmd_call_ctx(l->call);
    int64_t reply_to = ctx->msg != NULL ? ctx->msg->message_id : 0;
    if (res->status != 0) {
        (void)cmd_ctx_reply_plain(ctx, reply_to, "I could not read the blacklist.");
        free(l);
        return;
    }
    strbuf_t out;
    if (strbuf_init(&out, 256) != 0) {
        free(l);
        return;
    }
    (void)strbuf_appendf(&out, "Blacklist (%u):\n", res->blacklist_count);
    for (unsigned i = 0; i < res->blacklist_count; ++i) {
        (void)strbuf_appendf(&out, "%u. %s%s\n", i + 1, res->blacklist[i].is_regex != 0 ? "/" : "",
                             res->blacklist[i].pattern);
    }
    if (res->blacklist_count == 0) {
        (void)strbuf_append_str(&out, "(empty)");
    }
    (void)cmd_ctx_reply_plain(ctx, reply_to, strbuf_cstr(&out));
    strbuf_free(&out);
    free(l);
}

/**
 * @brief Blacklist add/del completion: reply with the outcome.
 *
 * @param[in] res  DB result.
 * @param[in] user The @ref automod_set_ctx_t (freed here).
 */
static void on_blacklist_mutated(const db_result_t *res, void *user)
{
    automod_set_ctx_t *s = user;
    struct cmd_ctx *ctx = cmd_call_ctx(s->call);
    int64_t reply_to = ctx->msg != NULL ? ctx->msg->message_id : 0;
    (void)cmd_ctx_reply_plain(ctx, reply_to,
                              res->status == 0 ? s->reply : "I could not update the blacklist.");
    free(s);
}

/**
 * @brief Submit a blacklist add/delete and reply on completion.
 *
 * @param[in] ctx   Context.
 * @param[in] add   1 to add, 0 to delete.
 * @param[in] pat   Pattern.
 * @param[in] regex 1 if the pattern is a regular expression.
 *
 * @return 0 if queued, negative errno on failure.
 */
static int blacklist_mutate(struct cmd_ctx *ctx, int add, const char *pat, int regex)
{
    if (ctx->db_submit == NULL) {
        (void)cmd_ctx_reply_plain(ctx, ctx->msg != NULL ? ctx->msg->message_id : 0,
                                  "The blacklist is unavailable right now.");
        return 0;
    }
    automod_set_ctx_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return -ENOMEM;
    }
    s->call = ctx->call;
    (void)snprintf(s->reply, sizeof(s->reply), add != 0 ? "Added to the blacklist." : "Removed.");
    db_job_t job = {.kind = add != 0 ? DB_JOB_BLACKLIST_ADD : DB_JOB_BLACKLIST_DEL,
                    .id = ctx->chat_id,
                    .value = regex};
    (void)snprintf(job.pattern, sizeof(job.pattern), "%s", pat);
    int rc = ctx->db_submit(ctx, &job, on_blacklist_mutated, s);
    if (rc != 0) {
        free(s);
    }
    return rc;
}

/**
 * @brief Parse the `/blacklist add|del <word|/regex/>` tail into an action+pattern.
 *
 * @param[in]  args      Command tail.
 * @param[out] out_add   1 for add, 0 for del.
 * @param[out] out_pat   Receives the (unquoted) pattern.
 * @param[in]  pat_cap   Capacity of @p out_pat.
 * @param[out] out_regex Receives 1 if the pattern is a regular expression.
 *
 * @return 0 on success, -EINVAL if the verb or pattern is missing/invalid.
 */
static int parse_blacklist_args(const char *args, int *out_add, char *out_pat, size_t pat_cap,
                                int *out_regex)
{
    int add;
    const char *rest;
    if (strncmp(args, "add ", 4) == 0) {
        add = 1;
        rest = args + 4;
    } else if (strncmp(args, "del ", 4) == 0) {
        add = 0;
        rest = args + 4;
    } else {
        return -EINVAL;
    }
    while (*rest == ' ') {
        rest++;
    }
    if (parse_pattern(rest, out_pat, pat_cap, out_regex) != 0) {
        return -EINVAL;
    }
    *out_add = add;
    return 0;
}

/**
 * @brief /blacklist handler: add, delete or list blacklist patterns.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_blacklist(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    int64_t reply_to = ctx->msg != NULL ? ctx->msg->message_id : 0;
    const char *args = ctx->args;
    if (args == NULL || args[0] == '\0') {
        (void)cmd_ctx_reply_plain(ctx, reply_to, "Usage: /blacklist add|del <word|/regex/> | list");
        return 0;
    }
    if (strncmp(args, "list", 4) == 0 && (args[4] == '\0' || args[4] == ' ')) {
        if (ctx->db_submit == NULL) {
            (void)cmd_ctx_reply_plain(ctx, reply_to, "The blacklist is unavailable right now.");
            return 0;
        }
        blacklist_list_ctx_t *l = calloc(1, sizeof(*l));
        if (l == NULL) {
            return -ENOMEM;
        }
        l->call = ctx->call;
        db_job_t job = {.kind = DB_JOB_BLACKLIST_LIST, .id = ctx->chat_id};
        int rc = ctx->db_submit(ctx, &job, on_blacklist_listed, l);
        if (rc != 0) {
            free(l);
        }
        return rc;
    }

    int add = 0;
    int regex = 0;
    char pat[DB_PATTERN_MAX];
    if (parse_blacklist_args(args, &add, pat, sizeof(pat), &regex) != 0) {
        (void)cmd_ctx_reply_plain(ctx, reply_to, "Usage: /blacklist add|del <word|/regex/> | list");
        return 0;
    }
    return blacklist_mutate(ctx, add, pat, regex);
}

/* --- /antiflood ------------------------------------------------------------ */

/**
 * @brief Parse two positive integers separated by whitespace from @p s.
 *
 * @param[in]  s   Input.
 * @param[out] a   First value.
 * @param[out] b   Second value.
 *
 * @return 0 on success, -EINVAL on malformed input or trailing garbage.
 */
static int parse_two_ints(const char *s, long *a, long *b)
{
    char *end = NULL;
    errno = 0;
    long x = strtol(s, &end, 10);
    if (end == s || errno == ERANGE || x <= 0) {
        return -EINVAL;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    const char *start2 = end;
    long y = strtol(start2, &end, 10);
    if (end == start2 || errno == ERANGE || y <= 0) {
        return -EINVAL;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end != '\0') {
        return -EINVAL;
    }
    *a = x;
    *b = y;
    return 0;
}

/**
 * @brief /antiflood handler: set the flood threshold or turn it off.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_antiflood(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    int64_t reply_to = ctx->msg != NULL ? ctx->msg->message_id : 0;
    const char *args = ctx->args;
    if (args != NULL && strcmp(args, "off") == 0) {
        return automod_write(ctx, DB_SET_FLOOD_LIMIT, 0, "Antiflood disabled.");
    }
    long n = 0;
    long secs = 0;
    if (parse_two_ints(args != NULL ? args : "", &n, &secs) != 0 || n > 1000 || secs > 86400) {
        (void)cmd_ctx_reply_plain(ctx, reply_to, "Usage: /antiflood <messages> <seconds> | off");
        return 0;
    }
    /* Two writes: threshold then window. The second replies. */
    if (ctx->db_submit == NULL) {
        (void)cmd_ctx_reply_plain(ctx, reply_to, "Settings are unavailable right now.");
        return 0;
    }
    db_job_t j1 = {.kind = DB_JOB_SETTINGS_SET,
                   .id = ctx->chat_id,
                   .settings_mask = DB_SET_FLOOD_LIMIT,
                   .value = n};
    if (ctx->db_submit(ctx, &j1, NULL, NULL) != 0) {
        return -ENOMEM;
    }
    char reply[96];
    (void)snprintf(reply, sizeof(reply), "Antiflood: %ld message(s) per %ld second(s).", n, secs);
    return automod_write(ctx, DB_SET_FLOOD_WINDOW, secs, reply);
}

/* --- /captcha -------------------------------------------------------------- */

/**
 * @brief /captcha handler: enable or disable join verification.
 *
 * @param[in] ctx Context.
 *
 * @return 0 if the command was handled, negative errno on a fatal error.
 */
int cmd_captcha(struct cmd_ctx *ctx)
{
    if (ctx == NULL) {
        return -EINVAL;
    }
    int64_t reply_to = ctx->msg != NULL ? ctx->msg->message_id : 0;
    if (ctx->args != NULL && strcmp(ctx->args, "on") == 0) {
        return automod_write(ctx, DB_SET_CAPTCHA_ON, 1, "Captcha enabled.");
    }
    if (ctx->args != NULL && strcmp(ctx->args, "off") == 0) {
        return automod_write(ctx, DB_SET_CAPTCHA_ON, 0, "Captcha disabled.");
    }
    (void)cmd_ctx_reply_plain(ctx, reply_to, "Usage: /captcha on|off");
    return 0;
}
