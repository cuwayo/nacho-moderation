/* test_cmd_settings.c - settings/welcome/rules handlers and helpers (A5.1).
 *
 * Covers the pure welcome renderer (every placeholder + escaping), the panel
 * callback parser and ownership rule, and the argument validation of the
 * settings commands over a fake API/DB seam. No network, no SQLite.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "api_methods.h"
#include "commands.h"
#include "tables.h"
#include "tg/api_pool.h"
#include "tg/cmd_ctx.h"
#include "tg/cmd_settings.h"
#include "tg/db.h"
#include "tg/tg_api.h"
#include "tg/tg_types.h"
#include "tg/util.h"

/* --- Fakes ----------------------------------------------------------------- */

typedef struct {
    int calls;
    method_id_t last_method;
    unsigned settings_writes;
    unsigned last_mask;
    int64_t last_value;
    char last_text[DB_TEXT_MAX];
    char last_body[4096];
} fake_t;

static int fake_request(void *ctx, method_id_t method, const char *body, size_t body_len,
                        int64_t chat_id, uint64_t deadline_ms, tg_api_cb cb, void *user)
{
    (void)chat_id;
    (void)deadline_ms;
    fake_t *f = ctx;
    f->calls++;
    f->last_method = method;
    size_t n = body_len < sizeof(f->last_body) - 1u ? body_len : sizeof(f->last_body) - 1u;
    if (body != NULL) {
        memcpy(f->last_body, body, n);
    }
    f->last_body[n] = '\0';
    if (cb != NULL) {
        api_result_t res = {0};
        cb(&res, user);
    }
    return 0;
}

static int fake_submit(void *ctx, const db_job_t *job, db_cb cb, void *user)
{
    if (job->kind == DB_JOB_SETTINGS_SET) {
        fake_t *f = ctx;
        f->settings_writes++;
        f->last_mask = job->settings_mask;
        f->last_value = job->value;
        (void)snprintf(f->last_text, sizeof(f->last_text), "%s", job->text);
    }
    if (cb != NULL) {
        db_result_t res = {0};
        cb(&res, user);
    }
    return 0;
}

static int64_t fake_now_unix(void *ctx)
{
    return *(int64_t *)ctx;
}

static cmd_call_t *make_call(const tg_api_ops_t *api, const db_ops_t *db,
                             const wall_clock_ops_t *wall, const char *name, const char *args,
                             const chat_settings_t *settings)
{
    cmd_call_t *call = cmd_call_create(api, db, wall);
    assert_non_null(call);
    struct cmd_ctx *ctx = cmd_call_ctx(call);
    tg_message_t *msg = cmd_call_message(call);
    msg->chat.id = -100;
    (void)snprintf(msg->chat.type, sizeof(msg->chat.type), "supergroup");
    msg->message_id = 42;
    msg->from.id = 200;
    ctx->cmd = cmd_lookup(name);
    ctx->chat_id = -100;
    ctx->user_id = 200;
    ctx->bot_id = 999;
    ctx->args = args;
    ctx->is_group = 1;
    ctx->role = ROLE_ADMIN;
    if (settings != NULL) {
        (void)cmd_ctx_set_settings(ctx, settings);
    }
    return call;
}

/* --- Welcome renderer ------------------------------------------------------ */

static void test_welcome_all_placeholders(void **state)
{
    (void)state;
    tg_user_t u = {.id = 7};
    (void)snprintf(u.first_name, sizeof(u.first_name), "Ada");
    (void)snprintf(u.username, sizeof(u.username), "ada");
    char out[256];
    assert_int_equal(cmd_render_welcome("Hi {first} in {chat} (#{count}) {mention}", &u, "Testers",
                                        42, out, sizeof(out)),
                     0);
    assert_string_equal(out, "Hi Ada in Testers (#42) <a href=\"https://t.me/ada\">Ada</a>");
}

static void test_welcome_escapes_names(void **state)
{
    (void)state;
    tg_user_t u = {.id = 7};
    (void)snprintf(u.first_name, sizeof(u.first_name), "<b>&");
    char out[128];
    assert_int_equal(cmd_render_welcome("{first}", &u, "", 0, out, sizeof(out)), 0);
    assert_string_equal(out, "&lt;b&gt;&amp;");
}

static void test_welcome_mention_without_username(void **state)
{
    (void)state;
    tg_user_t u = {.id = 7};
    (void)snprintf(u.first_name, sizeof(u.first_name), "Ada");
    char out[128];
    assert_int_equal(cmd_render_welcome("[{mention}]", &u, "", 0, out, sizeof(out)), 0);
    assert_string_equal(out, "[Ada]"); /* no username: a plain name, not a link */
}

static void test_welcome_unknown_token_and_short_buffer(void **state)
{
    (void)state;
    tg_user_t u = {.id = 7};
    (void)snprintf(u.first_name, sizeof(u.first_name), "Ada");
    char out[64];
    assert_int_equal(cmd_render_welcome("Hi {nope}", &u, "", 0, out, sizeof(out)), 0);
    assert_string_equal(out, "Hi {nope}"); /* unknown token copied through */

    char tiny[4];
    assert_int_equal(cmd_render_welcome("Hello", &u, "", 0, tiny, sizeof(tiny)), -ENOSPC);
    assert_int_equal(cmd_render_welcome(NULL, &u, NULL, 0, NULL, 0), -EINVAL);
}

/* --- Callback parser ------------------------------------------------------- */

static void test_callback_parse_ok(void **state)
{
    (void)state;
    int64_t chat = 0;
    int64_t user = 0;
    const char *key = NULL;
    assert_int_equal(cmd_settings_parse_callback("set:-100:200:welcome", &chat, &user, &key), 0);
    assert_int_equal(chat, -100);
    assert_int_equal(user, 200);
    assert_string_equal(key, "welcome");
}

static void test_callback_parse_rejects_bad(void **state)
{
    (void)state;
    int64_t chat = 0;
    int64_t user = 0;
    assert_int_not_equal(cmd_settings_parse_callback("cap:1:2:3", &chat, &user, NULL), 0);
    assert_int_not_equal(cmd_settings_parse_callback("set:-100:200", &chat, &user, NULL), 0);
    assert_int_not_equal(cmd_settings_parse_callback("set::200:welcome", &chat, &user, NULL), 0);
    assert_int_not_equal(cmd_settings_parse_callback(NULL, &chat, &user, NULL), 0);
}

/* --- Settings commands ----------------------------------------------------- */

static void test_setwarnlimit_validates_and_writes(void **state)
{
    (void)state;
    fake_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    db_ops_t db = {.submit = fake_submit, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *c1 = make_call(&api, &db, &wall, "setwarnlimit", "5", NULL);
    assert_int_equal(cmd_setwarnlimit(cmd_call_ctx(c1)), 0);
    assert_int_equal(f.settings_writes, 1);
    assert_int_equal(f.last_mask, DB_SET_WARN_LIMIT);
    assert_int_equal(f.last_value, 5);
    cmd_call_release(c1);

    /* Garbage is rejected without a write. */
    cmd_call_t *c2 = make_call(&api, &db, &wall, "setwarnlimit", "lots", NULL);
    assert_int_equal(cmd_setwarnlimit(cmd_call_ctx(c2)), 0);
    assert_int_equal(f.settings_writes, 1);
    assert_int_equal(f.last_method, METHOD_SEND_MESSAGE);
    cmd_call_release(c2);
}

static void test_setwarnaction_validates(void **state)
{
    (void)state;
    fake_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    db_ops_t db = {.submit = fake_submit, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *c1 = make_call(&api, &db, &wall, "setwarnaction", "ban", NULL);
    assert_int_equal(cmd_setwarnaction(cmd_call_ctx(c1)), 0);
    assert_int_equal(f.settings_writes, 1);
    assert_int_equal(f.last_mask, DB_SET_WARN_ACTION);
    assert_string_equal(f.last_text, "ban");
    cmd_call_release(c1);

    cmd_call_t *c2 = make_call(&api, &db, &wall, "setwarnaction", "explode", NULL);
    assert_int_equal(cmd_setwarnaction(cmd_call_ctx(c2)), 0);
    assert_int_equal(f.settings_writes, 1); /* rejected, no write */
    cmd_call_release(c2);
}

static void test_setlog_off_and_id(void **state)
{
    (void)state;
    fake_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    db_ops_t db = {.submit = fake_submit, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *c1 = make_call(&api, &db, &wall, "setlog", "-100999", NULL);
    assert_int_equal(cmd_setlog(cmd_call_ctx(c1)), 0);
    assert_int_equal(f.last_mask, DB_SET_LOG_CHAT);
    assert_int_equal(f.last_value, -100999);
    cmd_call_release(c1);

    cmd_call_t *c2 = make_call(&api, &db, &wall, "setlog", "off", NULL);
    assert_int_equal(cmd_setlog(cmd_call_ctx(c2)), 0);
    assert_int_equal(f.last_value, 0);
    cmd_call_release(c2);
}

static void test_welcome_on_off(void **state)
{
    (void)state;
    fake_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    db_ops_t db = {.submit = fake_submit, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    cmd_call_t *c1 = make_call(&api, &db, &wall, "welcome", "on", NULL);
    assert_int_equal(cmd_welcome(cmd_call_ctx(c1)), 0);
    assert_int_equal(f.last_mask, DB_SET_WELCOME_ON);
    assert_int_equal(f.last_value, 1);
    cmd_call_release(c1);

    cmd_call_t *c2 = make_call(&api, &db, &wall, "welcome", "off", NULL);
    assert_int_equal(cmd_welcome(cmd_call_ctx(c2)), 0);
    assert_int_equal(f.last_value, 0);
    cmd_call_release(c2);
}

static void test_rules_shows_text_or_note(void **state)
{
    (void)state;
    fake_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    chat_settings_t s = {0};
    (void)snprintf(s.rules_text, sizeof(s.rules_text), "Be kind.");
    cmd_call_t *c1 = make_call(&api, NULL, &wall, "rules", "", &s);
    assert_int_equal(cmd_rules(cmd_call_ctx(c1)), 0);
    assert_non_null(strstr(f.last_body, "Be kind"));
    cmd_call_release(c1);

    cmd_call_t *c2 = make_call(&api, NULL, &wall, "rules", "", NULL);
    assert_int_equal(cmd_rules(cmd_call_ctx(c2)), 0);
    assert_non_null(strstr(f.last_body, "No rules"));
    cmd_call_release(c2);
}

static void test_settings_panel_has_keyboard(void **state)
{
    (void)state;
    fake_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    chat_settings_t s = {0};
    s.welcome_enabled = 1;
    cmd_call_t *call = make_call(&api, NULL, &wall, "settings", "", &s);
    assert_int_equal(cmd_settings(cmd_call_ctx(call)), 0);
    assert_int_equal(f.last_method, METHOD_SEND_MESSAGE);
    assert_non_null(strstr(f.last_body, "inline_keyboard"));
    assert_non_null(strstr(f.last_body, "Welcome: ON"));
    assert_non_null(strstr(f.last_body, "set:-100:200:welcome"));
    cmd_call_release(call);
}

static int g_admin_invalidated;
static void count_invalidator(void *impl, int64_t chat_id)
{
    (void)impl;
    (void)chat_id;
    g_admin_invalidated++;
}

static void test_reload_invalidates_admins(void **state)
{
    (void)state;
    fake_t f = {0};
    tg_api_ops_t api = {.request = fake_request, .ctx = &f};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};

    g_admin_invalidated = 0;
    cmd_call_t *call = make_call(&api, NULL, &wall, "reload", "", NULL);
    cmd_call_set_admin_invalidator(call, count_invalidator, NULL);
    assert_int_equal(cmd_reload(cmd_call_ctx(call)), 0);
    assert_int_equal(g_admin_invalidated, 1);
    assert_int_equal(f.last_method, METHOD_SEND_MESSAGE);
    cmd_call_release(call);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_welcome_all_placeholders),
        cmocka_unit_test(test_welcome_escapes_names),
        cmocka_unit_test(test_welcome_mention_without_username),
        cmocka_unit_test(test_welcome_unknown_token_and_short_buffer),
        cmocka_unit_test(test_callback_parse_ok),
        cmocka_unit_test(test_callback_parse_rejects_bad),
        cmocka_unit_test(test_setwarnlimit_validates_and_writes),
        cmocka_unit_test(test_setwarnaction_validates),
        cmocka_unit_test(test_setlog_off_and_id),
        cmocka_unit_test(test_welcome_on_off),
        cmocka_unit_test(test_rules_shows_text_or_note),
        cmocka_unit_test(test_settings_panel_has_keyboard),
        cmocka_unit_test(test_reload_invalidates_admins),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
