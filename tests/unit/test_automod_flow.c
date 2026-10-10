/* test_automod_flow.c - the full automod pipeline over fake seams (A5.1).
 *
 * Drives automod_handle_message through a real settings service and permission
 * engine over fake DB/API seams: the locks path (delete + action), the blacklist
 * path (delete + warn), the antiflood path (mute + burst delete), the
 * administrator exemption and the bot skip. No network, no SQLite.
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
#include "tg/api_pool.h"
#include "tg/automod.h"
#include "tg/db.h"
#include "tg/perms.h"
#include "tg/settings.h"
#include "tg/tg_api.h"
#include "tg/tg_types.h"
#include "tg/util.h"

/* --- Fakes ----------------------------------------------------------------- */

typedef struct {
    int deletes;
    int restricts;
    int sends;
    int bans;
    method_id_t last_method;
    char last_body[1024];
} fake_api_t;

/* The admin list JSON: creator id 1, admin id 200, the bot 999. */
static const char *ADMINS_JSON =
    "{\"ok\":true,\"result\":["
    "{\"status\":\"creator\",\"user\":{\"id\":1,\"first_name\":\"Own\"}},"
    "{\"status\":\"administrator\",\"user\":{\"id\":200,"
    "\"first_name\":\"Adm\"},"
    "\"can_delete_messages\":true,\"can_restrict_members\":true}]}";

static int fake_api_request(void *ctx, method_id_t method, const char *body, size_t body_len,
                            int64_t chat_id, uint64_t deadline_ms, tg_api_cb cb, void *user)
{
    (void)chat_id;
    (void)deadline_ms;
    fake_api_t *f = ctx;
    f->last_method = method;
    if (method == METHOD_DELETE_MESSAGE) {
        f->deletes++;
    } else if (method == METHOD_RESTRICT_CHAT_MEMBER) {
        f->restricts++;
    } else if (method == METHOD_SEND_MESSAGE) {
        f->sends++;
    } else if (method == METHOD_BAN_CHAT_MEMBER) {
        f->bans++;
    }
    size_t n = body_len < sizeof(f->last_body) - 1u ? body_len : sizeof(f->last_body) - 1u;
    if (body != NULL) {
        memcpy(f->last_body, body, n);
    }
    f->last_body[n] = '\0';
    if (cb != NULL) {
        api_result_t res = {0};
        /* getChatAdministrators carries the admin list. */
        res.body = ADMINS_JSON;
        res.body_len = strlen(ADMINS_JSON);
        cb(&res, user);
    }
    return 0;
}

typedef struct {
    chat_settings_t settings; /**< Returned from every SETTINGS_GET. */
    int settings_gets;
    int blacklist_lists;
    db_blacklist_t blacklist[DB_MAX_BLACKLIST];
    unsigned blacklist_count;
    int warn_adds;
    int64_t warn_count; /**< Returned from a WARN_ADD. */
} fake_db_t;

static int fake_db_submit(void *ctx, const db_job_t *job, db_cb cb, void *user)
{
    fake_db_t *d = ctx;
    db_result_t res = {0};
    switch (job->kind) {
    case DB_JOB_SETTINGS_GET:
        d->settings_gets++;
        res.settings = d->settings;
        break;
    case DB_JOB_BLACKLIST_LIST:
        d->blacklist_lists++;
        res.blacklist_count = d->blacklist_count;
        for (unsigned i = 0; i < d->blacklist_count; ++i) {
            res.blacklist[i] = d->blacklist[i];
        }
        break;
    case DB_JOB_WARN_ADD:
        d->warn_adds++;
        res.i64 = d->warn_count;
        break;
    default:
        break;
    }
    if (cb != NULL) {
        cb(&res, user);
    }
    return 0;
}

static uint64_t fake_now_ms(void *ctx)
{
    return *(uint64_t *)ctx;
}

static int64_t fake_now_unix(void *ctx)
{
    return *(int64_t *)ctx;
}

/** @brief Build a group message from @p user with @p text. */
static tg_message_t make_msg(int64_t chat, int64_t user, const char *text)
{
    tg_message_t m = {0};
    m.chat.id = chat;
    (void)snprintf(m.chat.type, sizeof(m.chat.type), "supergroup");
    (void)snprintf(m.chat.title, sizeof(m.chat.title), "Testers");
    m.from.id = user;
    (void)snprintf(m.from.first_name, sizeof(m.from.first_name), "User");
    m.message_id = 42;
    (void)snprintf(m.text, sizeof(m.text), "%s", text);
    return m;
}

/** @brief A test fixture wiring the real services over the fakes. */
typedef struct {
    fake_api_t api;
    fake_db_t db;
    tg_api_ops_t apiops;
    db_ops_t dbops;
    uint64_t now_ms;
    int64_t now_unix;
    clock_ops_t clock;
    wall_clock_ops_t wall;
    settings_t *settings;
    perms_t *perms;
    automod_t *automod;
} fixture_t;

static void fixture_init(fixture_t *fx)
{
    memset(fx, 0, sizeof(*fx));
    fx->apiops.request = fake_api_request;
    fx->apiops.ctx = &fx->api;
    fx->dbops.submit = fake_db_submit;
    fx->dbops.ctx = &fx->db;
    fx->now_ms = 1000;
    fx->now_unix = 1700000000;
    fx->clock.now_ms = fake_now_ms;
    fx->clock.ctx = &fx->now_ms;
    fx->wall.now_unix = fake_now_unix;
    fx->wall.ctx = &fx->now_unix;

    settings_config_t scfg = {.db = &fx->dbops, .clock = &fx->clock};
    fx->settings = settings_create(&scfg);
    assert_non_null(fx->settings);

    perms_config_t pcfg = {.clock = &fx->clock, .api = &fx->apiops, .bot_id = 999};
    fx->perms = perms_create(&pcfg);
    assert_non_null(fx->perms);

    automod_config_t acfg = {.api = &fx->apiops,
                             .db = &fx->dbops,
                             .settings = fx->settings,
                             .perms = fx->perms,
                             .clock = &fx->clock,
                             .wall = &fx->wall,
                             .bot_id = 999};
    fx->automod = automod_create(&acfg);
    assert_non_null(fx->automod);
}

static void fixture_free(fixture_t *fx)
{
    automod_destroy(fx->automod);
    perms_destroy(fx->perms);
    settings_destroy(fx->settings);
}

/* --- Locks ----------------------------------------------------------------- */

static void test_lock_deletes_and_records(void **state)
{
    (void)state;
    fixture_t fx;
    fixture_init(&fx);
    fx.db.settings.locks = LOCK_MEDIA;

    tg_message_t m = make_msg(-100, 300, "");
    m.content_flags = TG_CONTENT_MEDIA;
    assert_int_equal(automod_handle_message(fx.automod, &m), 0);
    assert_int_equal(fx.api.deletes, 1);
    assert_int_equal(fx.api.last_method, METHOD_DELETE_MESSAGE);
    fixture_free(&fx);
}

static void test_lock_admin_exempt(void **state)
{
    (void)state;
    fixture_t fx;
    fixture_init(&fx);
    fx.db.settings.locks = LOCK_MEDIA;

    /* user 200 is an administrator in the fake admin list. */
    tg_message_t m = make_msg(-100, 200, "");
    m.content_flags = TG_CONTENT_MEDIA;
    assert_int_equal(automod_handle_message(fx.automod, &m), 0);
    assert_int_equal(fx.api.deletes, 0);
    fixture_free(&fx);
}

static void test_bot_ignored_unless_locked(void **state)
{
    (void)state;
    fixture_t fx;
    fixture_init(&fx);
    fx.db.settings.locks = LOCK_MEDIA; /* bots not locked */

    tg_message_t m = make_msg(-100, 300, "hi");
    m.from.is_bot = 1;
    m.content_flags = TG_CONTENT_MEDIA;
    assert_int_equal(automod_handle_message(fx.automod, &m), 0);
    assert_int_equal(fx.api.deletes, 0); /* a bot is skipped */
    fixture_free(&fx);
}

static void test_bot_deleted_when_locked(void **state)
{
    (void)state;
    fixture_t fx;
    fixture_init(&fx);
    fx.db.settings.locks = LOCK_BOTS;

    tg_message_t m = make_msg(-100, 300, "hi");
    m.from.is_bot = 1;
    assert_int_equal(automod_handle_message(fx.automod, &m), 0);
    assert_int_equal(fx.api.deletes, 1);
    fixture_free(&fx);
}

/* --- Blacklist ------------------------------------------------------------- */

static void test_blacklist_deletes_and_warns(void **state)
{
    (void)state;
    fixture_t fx;
    fixture_init(&fx);
    fx.db.blacklist_count = 1;
    (void)snprintf(fx.db.blacklist[0].pattern, sizeof(fx.db.blacklist[0].pattern), "scam");
    fx.db.warn_count = 1;

    tg_message_t m = make_msg(-100, 300, "this is a SCAM");
    assert_int_equal(automod_handle_message(fx.automod, &m), 0);
    assert_int_equal(fx.api.deletes, 1);
    assert_int_equal(fx.db.warn_adds, 1);
    fixture_free(&fx);
}

static void test_blacklist_no_match_passes(void **state)
{
    (void)state;
    fixture_t fx;
    fixture_init(&fx);
    fx.db.blacklist_count = 1;
    (void)snprintf(fx.db.blacklist[0].pattern, sizeof(fx.db.blacklist[0].pattern), "scam");

    tg_message_t m = make_msg(-100, 300, "totally fine");
    assert_int_equal(automod_handle_message(fx.automod, &m), 0);
    assert_int_equal(fx.api.deletes, 0);
    assert_int_equal(fx.db.warn_adds, 0);
    fixture_free(&fx);
}

static void test_blacklist_patterns_cached_between_messages(void **state)
{
    (void)state;
    fixture_t fx;
    fixture_init(&fx);
    fx.db.blacklist_count = 1;
    (void)snprintf(fx.db.blacklist[0].pattern, sizeof(fx.db.blacklist[0].pattern), "scam");

    /* The first message fetches the patterns; the second is served from cache. */
    tg_message_t m1 = make_msg(-100, 300, "SCAM");
    assert_int_equal(automod_handle_message(fx.automod, &m1), 0);
    int lists_after_first = fx.db.blacklist_lists;
    tg_message_t m2 = make_msg(-100, 301, "another SCAM");
    assert_int_equal(automod_handle_message(fx.automod, &m2), 0);
    assert_int_equal(fx.db.blacklist_lists, lists_after_first); /* cache hit */
    assert_int_equal(fx.api.deletes, 2);
    fixture_free(&fx);
}

static void test_sender_label_username_fallback(void **state)
{
    (void)state;
    fixture_t fx;
    fixture_init(&fx);
    fx.db.settings.locks = LOCK_MEDIA;

    /* No first name, but a username: the label falls back to @username. */
    tg_message_t m = make_msg(-100, 300, "");
    m.from.first_name[0] = '\0';
    (void)snprintf(m.from.username, sizeof(m.from.username), "spammer");
    m.content_flags = TG_CONTENT_MEDIA;
    assert_int_equal(automod_handle_message(fx.automod, &m), 0);
    assert_int_equal(fx.api.deletes, 1);
    fixture_free(&fx);
}

static void test_blacklist_warn_limit_applies_action(void **state)
{
    (void)state;
    fixture_t fx;
    fixture_init(&fx);
    fx.db.blacklist_count = 1;
    (void)snprintf(fx.db.blacklist[0].pattern, sizeof(fx.db.blacklist[0].pattern), "scam");
    /* The warn action fires: the count from WARN_ADD reaches the limit. */
    fx.db.settings.warn_limit = 1;
    (void)snprintf(fx.db.settings.warn_action, sizeof(fx.db.settings.warn_action), "mute");
    fx.db.warn_count = 1;

    tg_message_t m = make_msg(-100, 300, "SCAM");
    assert_int_equal(automod_handle_message(fx.automod, &m), 0);
    assert_int_equal(fx.db.warn_adds, 1);
    /* The auto-action muted the user. */
    assert_true(fx.api.restricts >= 1);
    fixture_free(&fx);
}

static void test_blacklist_warn_limit_ban_action(void **state)
{
    (void)state;
    fixture_t fx;
    fixture_init(&fx);
    fx.db.blacklist_count = 1;
    (void)snprintf(fx.db.blacklist[0].pattern, sizeof(fx.db.blacklist[0].pattern), "scam");
    fx.db.settings.warn_limit = 1;
    (void)snprintf(fx.db.settings.warn_action, sizeof(fx.db.settings.warn_action), "ban");
    fx.db.warn_count = 1;

    tg_message_t m = make_msg(-100, 300, "SCAM");
    assert_int_equal(automod_handle_message(fx.automod, &m), 0);
    /* The ban path issued a banChatMember. */
    assert_true(fx.api.bans >= 1);
    fixture_free(&fx);
}

/* --- Antiflood ------------------------------------------------------------- */

static void test_antiflood_mutes_burst(void **state)
{
    (void)state;
    fixture_t fx;
    fixture_init(&fx);
    fx.db.settings.flood_limit = 2;
    fx.db.settings.flood_window = 10;

    for (int i = 0; i < 3; ++i) {
        tg_message_t m = make_msg(-100, 300, "spam");
        m.message_id = 40 + i;
        fx.now_ms += 100; /* all within the window */
        assert_int_equal(automod_handle_message(fx.automod, &m), 0);
    }
    /* The third message exceeds the limit: mute + delete the burst. */
    assert_int_equal(fx.api.restricts, 1);
    assert_true(fx.api.deletes >= 1);
    fixture_free(&fx);
}

static void test_antiflood_window_expires(void **state)
{
    (void)state;
    fixture_t fx;
    fixture_init(&fx);
    fx.db.settings.flood_limit = 2;
    fx.db.settings.flood_window = 1;

    for (int i = 0; i < 3; ++i) {
        tg_message_t m = make_msg(-100, 300, "spam");
        m.message_id = 40 + i;
        fx.now_ms += 2000; /* each outside the 1 s window */
        assert_int_equal(automod_handle_message(fx.automod, &m), 0);
    }
    assert_int_equal(fx.api.restricts, 0); /* never exceeded the window */
    fixture_free(&fx);
}

/* --- Guards ---------------------------------------------------------------- */

static void test_private_chat_ignored(void **state)
{
    (void)state;
    fixture_t fx;
    fixture_init(&fx);
    fx.db.settings.locks = LOCK_MEDIA;

    tg_message_t m = make_msg(5, 300, "");
    (void)snprintf(m.chat.type, sizeof(m.chat.type), "private");
    m.content_flags = TG_CONTENT_MEDIA;
    assert_int_equal(automod_handle_message(fx.automod, &m), 0);
    assert_int_equal(fx.api.deletes, 0);
    fixture_free(&fx);
}

static void test_bad_args(void **state)
{
    (void)state;
    assert_int_equal(automod_handle_message(NULL, NULL), -EINVAL);
    assert_null(automod_create(NULL));
    automod_destroy(NULL); /* no-op */
}

static void test_without_perms_engine_still_locks(void **state)
{
    (void)state;
    /* No permission engine: admins cannot be resolved, so the pipeline runs for
     * every non-bot sender. */
    fake_api_t api = {0};
    fake_db_t db = {0};
    db.settings.locks = LOCK_MEDIA;
    tg_api_ops_t apiops = {.request = fake_api_request, .ctx = &api};
    db_ops_t dbops = {.submit = fake_db_submit, .ctx = &db};
    uint64_t now_ms = 1000;
    int64_t now_unix = 1700000000;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now_ms};
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now_unix};
    settings_config_t scfg = {.db = &dbops, .clock = &clock};
    settings_t *settings = settings_create(&scfg);
    automod_config_t acfg = {.api = &apiops,
                             .db = &dbops,
                             .settings = settings,
                             .perms = NULL,
                             .clock = &clock,
                             .wall = &wall,
                             .bot_id = 999};
    automod_t *am = automod_create(&acfg);
    assert_non_null(am);

    tg_message_t m = make_msg(-100, 300, "");
    m.content_flags = TG_CONTENT_MEDIA;
    assert_int_equal(automod_handle_message(am, &m), 0);
    assert_int_equal(api.deletes, 1);
    automod_destroy(am);
    settings_destroy(settings);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_lock_deletes_and_records),
        cmocka_unit_test(test_lock_admin_exempt),
        cmocka_unit_test(test_bot_ignored_unless_locked),
        cmocka_unit_test(test_bot_deleted_when_locked),
        cmocka_unit_test(test_blacklist_deletes_and_warns),
        cmocka_unit_test(test_blacklist_no_match_passes),
        cmocka_unit_test(test_blacklist_patterns_cached_between_messages),
        cmocka_unit_test(test_sender_label_username_fallback),
        cmocka_unit_test(test_blacklist_warn_limit_applies_action),
        cmocka_unit_test(test_blacklist_warn_limit_ban_action),
        cmocka_unit_test(test_antiflood_mutes_burst),
        cmocka_unit_test(test_antiflood_window_expires),
        cmocka_unit_test(test_private_chat_ignored),
        cmocka_unit_test(test_without_perms_engine_still_locks),
        cmocka_unit_test(test_bad_args),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
