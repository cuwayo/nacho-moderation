/* test_captcha.c - join verification: parser, flow and timeout sweep (A5.1).
 *
 * Drives src/captcha.c through fake DB/API/clock/random seams: the join path
 * (restrict + prompt), the callback verification (correct user lifts; wrong user
 * rejected), the nonce format and the timeout sweep (kick + cleanup). No
 * network, no SQLite, no real clock or randomness.
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
#include "fake_ring.h"
#include "tg/api_pool.h"
#include "tg/captcha.h"
#include "tg/db.h"
#include "tg/loop.h"
#include "tg/settings.h"
#include "tg/tg_api.h"
#include "tg/tg_types.h"
#include "tg/util.h"

/* --- Fakes ----------------------------------------------------------------- */

typedef struct {
    int captcha_put;
    int captcha_del;
    int captcha_get;
    int expire_queries;
    int settings_gets;
    chat_settings_t settings; /**< Returned from every SETTINGS_GET. */
    db_result_t scripted;     /**< Returned from the next get/expire query. */
} fake_db_t;

static int fake_submit(void *ctx, const db_job_t *job, db_cb cb, void *user)
{
    fake_db_t *d = ctx;
    db_result_t res = {0};
    switch (job->kind) {
    case DB_JOB_CAPTCHA_PUT:
        d->captcha_put++;
        break;
    case DB_JOB_CAPTCHA_DEL:
        d->captcha_del++;
        break;
    case DB_JOB_CAPTCHA_GET:
        d->captcha_get++;
        res = d->scripted;
        break;
    case DB_JOB_CAPTCHA_EXPIRE:
        d->expire_queries++;
        res = d->scripted;
        break;
    case DB_JOB_SETTINGS_GET:
        d->settings_gets++;
        res.settings = d->settings;
        break;
    default:
        break;
    }
    if (cb != NULL) {
        cb(&res, user);
    }
    return 0;
}

typedef struct {
    int calls;
    method_id_t last_method;
    int send_messages;
    int restricts;
    int bans;
    int unbans;
    int deletes;
    char last_body[1024];
} fake_api_t;

static int fake_request(void *ctx, method_id_t method, const char *body, size_t body_len,
                        int64_t chat_id, uint64_t deadline_ms, tg_api_cb cb, void *user)
{
    (void)chat_id;
    (void)deadline_ms;
    fake_api_t *f = ctx;
    f->calls++;
    f->last_method = method;
    switch (method) {
    case METHOD_SEND_MESSAGE:
        f->send_messages++;
        break;
    case METHOD_RESTRICT_CHAT_MEMBER:
        f->restricts++;
        break;
    case METHOD_BAN_CHAT_MEMBER:
        f->bans++;
        break;
    case METHOD_UNBAN_CHAT_MEMBER:
        f->unbans++;
        break;
    case METHOD_DELETE_MESSAGE:
        f->deletes++;
        break;
    default:
        break;
    }
    size_t n = body_len < sizeof(f->last_body) - 1u ? body_len : sizeof(f->last_body) - 1u;
    if (body != NULL) {
        memcpy(f->last_body, body, n);
    }
    f->last_body[n] = '\0';
    if (cb != NULL) {
        api_result_t res = {0};
        /* A sendMessage reply carries the prompt message id. */
        res.body = "{\"ok\":true,\"result\":{\"message_id\":555}}";
        res.body_len = strlen(res.body);
        cb(&res, user);
    }
    return 0;
}

static int64_t fake_now_unix(void *ctx)
{
    return *(int64_t *)ctx;
}

static uint64_t fake_next_u64(void *ctx)
{
    (void)ctx;
    return 0xDEADBEEFCAFEBABEULL;
}

/* --- Parser ---------------------------------------------------------------- */

static void test_parse_callback_ok(void **state)
{
    (void)state;
    int64_t chat = 0;
    int64_t user = 0;
    char nonce[32];
    assert_int_equal(
        captcha_parse_callback("cap:-100123:42:deadbeef", &chat, &user, nonce, sizeof(nonce)), 0);
    assert_int_equal(chat, -100123);
    assert_int_equal(user, 42);
    assert_string_equal(nonce, "deadbeef");
}

static void test_parse_callback_rejects(void **state)
{
    (void)state;
    int64_t chat = 0;
    int64_t user = 0;
    char nonce[32];
    assert_int_equal(captcha_parse_callback("set:1:2:x", &chat, &user, nonce, sizeof(nonce)),
                     -EINVAL);
    assert_int_equal(captcha_parse_callback("cap:1:2", &chat, &user, nonce, sizeof(nonce)),
                     -EINVAL);
    assert_int_equal(captcha_parse_callback("cap:1:2:", &chat, &user, nonce, sizeof(nonce)),
                     -EINVAL);
    assert_int_equal(captcha_parse_callback(NULL, &chat, &user, nonce, sizeof(nonce)), -EINVAL);
    /* A nonce longer than the buffer is rejected, not truncated. */
    assert_int_equal(captcha_parse_callback("cap:1:2:toolongnonce", &chat, &user, nonce, 4),
                     -EINVAL);
}

/* --- Callback handling ----------------------------------------------------- */

static void test_callback_wrong_user_rejected(void **state)
{
    (void)state;
    fake_db_t db = {0};
    db_ops_t dbops = {.submit = fake_submit, .ctx = &db};
    fake_api_t api = {0};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = &api};
    int64_t now = 1700000000;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};
    random_ops_t rnd = {.next_u64 = fake_next_u64, .ctx = NULL};
    fake_ring_t ring;
    const loop_ops_t *lops = fake_ring_init(&ring);
    captcha_config_t cfg = {
        .ops = lops, .api = &apiops, .db = &dbops, .wall = &wall, .random = &rnd};
    captcha_t *cap = captcha_create(&cfg);
    assert_non_null(cap);

    tg_update_t upd = {0};
    (void)snprintf(upd.callback_data, sizeof(upd.callback_data), "cap:-100:42:abcd");
    upd.callback_from.id = 999; /* not the challenged user */
    (void)snprintf(upd.callback_id, sizeof(upd.callback_id), "cb-1");
    assert_int_equal(captcha_handle_callback(cap, &upd), 1);
    assert_int_equal(api.last_method, METHOD_ANSWER_CALLBACK_QUERY);
    assert_int_equal(db.captcha_get, 0); /* never consulted the DB */
    captcha_destroy(cap);
}

static void test_callback_correct_user_lifts(void **state)
{
    (void)state;
    fake_db_t db = {0};
    db.scripted.status = 0;
    db.scripted.captcha_found = 1;
    (void)snprintf(db.scripted.captcha[0].nonce, sizeof(db.scripted.captcha[0].nonce), "abcd");
    db_ops_t dbops = {.submit = fake_submit, .ctx = &db};
    fake_api_t api = {0};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = &api};
    int64_t now = 1700000000;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};
    random_ops_t rnd = {.next_u64 = fake_next_u64, .ctx = NULL};
    fake_ring_t ring;
    const loop_ops_t *lops = fake_ring_init(&ring);
    captcha_config_t cfg = {
        .ops = lops, .api = &apiops, .db = &dbops, .wall = &wall, .random = &rnd};
    captcha_t *cap = captcha_create(&cfg);

    tg_update_t upd = {0};
    (void)snprintf(upd.callback_data, sizeof(upd.callback_data), "cap:-100:42:abcd");
    upd.callback_from.id = 42;
    upd.callback_message_id = 7;
    (void)snprintf(upd.callback_id, sizeof(upd.callback_id), "cb-2");
    assert_int_equal(captcha_handle_callback(cap, &upd), 1);
    assert_int_equal(db.captcha_get, 1);
    /* Lifted the restriction, deleted the prompt and dropped the row. */
    assert_true(api.restricts >= 1);
    assert_true(api.deletes >= 1);
    assert_int_equal(db.captcha_del, 1);
    captcha_destroy(cap);
}

static void test_callback_nonce_mismatch_rejected(void **state)
{
    (void)state;
    fake_db_t db = {0};
    db.scripted.status = 0;
    db.scripted.captcha_found = 1;
    (void)snprintf(db.scripted.captcha[0].nonce, sizeof(db.scripted.captcha[0].nonce), "other");
    db_ops_t dbops = {.submit = fake_submit, .ctx = &db};
    fake_api_t api = {0};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = &api};
    int64_t now = 1700000000;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};
    random_ops_t rnd = {.next_u64 = fake_next_u64, .ctx = NULL};
    fake_ring_t ring;
    const loop_ops_t *lops = fake_ring_init(&ring);
    captcha_config_t cfg = {
        .ops = lops, .api = &apiops, .db = &dbops, .wall = &wall, .random = &rnd};
    captcha_t *cap = captcha_create(&cfg);

    tg_update_t upd = {0};
    (void)snprintf(upd.callback_data, sizeof(upd.callback_data), "cap:-100:42:abcd");
    upd.callback_from.id = 42;
    (void)snprintf(upd.callback_id, sizeof(upd.callback_id), "cb-3");
    assert_int_equal(captcha_handle_callback(cap, &upd), 1);
    /* Nonce mismatch: no lift, no row delete. */
    assert_int_equal(db.captcha_del, 0);
    captcha_destroy(cap);
}

/* --- Timeout sweep --------------------------------------------------------- */

static void test_sweep_kicks_due_members(void **state)
{
    (void)state;
    fake_db_t db = {0};
    db.scripted.status = 0;
    db.scripted.captcha_count = 2;
    db.scripted.captcha[0].chat_id = -100;
    db.scripted.captcha[0].user_id = 42;
    db.scripted.captcha[0].message_id = 7;
    db.scripted.captcha[1].chat_id = -100;
    db.scripted.captcha[1].user_id = 43;
    db.scripted.captcha[1].message_id = 8;
    db_ops_t dbops = {.submit = fake_submit, .ctx = &db};
    fake_api_t api = {0};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = &api};
    int64_t now = 1700000000;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};
    random_ops_t rnd = {.next_u64 = fake_next_u64, .ctx = NULL};
    fake_ring_t ring;
    const loop_ops_t *lops = fake_ring_init(&ring);
    captcha_config_t cfg = {
        .ops = lops, .api = &apiops, .db = &dbops, .wall = &wall, .random = &rnd};
    captcha_t *cap = captcha_create(&cfg);

    assert_int_equal(captcha_sweep_now(cap), 0);
    assert_int_equal(db.expire_queries, 1);
    /* Each due member: ban + unban (kick) and a prompt delete; rows dropped. */
    assert_int_equal(api.bans, 2);
    assert_int_equal(api.unbans, 2);
    assert_int_equal(db.captcha_del, 2);
    captcha_destroy(cap);
}

static void test_sweep_empty(void **state)
{
    (void)state;
    fake_db_t db = {0};
    db.scripted.captcha_count = 0;
    db_ops_t dbops = {.submit = fake_submit, .ctx = &db};
    fake_api_t api = {0};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = &api};
    int64_t now = 0;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};
    random_ops_t rnd = {.next_u64 = fake_next_u64, .ctx = NULL};
    fake_ring_t ring;
    const loop_ops_t *lops = fake_ring_init(&ring);
    captcha_config_t cfg = {
        .ops = lops, .api = &apiops, .db = &dbops, .wall = &wall, .random = &rnd};
    captcha_t *cap = captcha_create(&cfg);
    assert_int_equal(captcha_sweep_now(cap), 0);
    assert_int_equal(api.calls, 0);
    captcha_destroy(cap);
}

/* --- Join flow ------------------------------------------------------------- */

/** @brief Record a welcome delivery (the captcha's on_welcome callback). */
static int g_welcomed;

static void fake_on_welcome(void *ctx, const tg_chat_t *chat, const tg_user_t *user)
{
    (void)ctx;
    (void)chat;
    (void)user;
    g_welcomed++;
}

static void test_join_challenges_when_enabled(void **state)
{
    (void)state;
    g_welcomed = 0;
    fake_db_t db = {0};
    db.settings.captcha_enabled = 1;
    db.settings.captcha_timeout = 90;
    db_ops_t dbops = {.submit = fake_submit, .ctx = &db};
    fake_api_t api = {0};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = &api};
    int64_t now = 1700000000;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};
    random_ops_t rnd = {.next_u64 = fake_next_u64, .ctx = NULL};
    fake_ring_t ring;
    const loop_ops_t *lops = fake_ring_init(&ring);
    settings_config_t scfg = {.db = &dbops, .clock = &util_clock_ops_real};
    settings_t *settings = settings_create(&scfg);
    captcha_config_t cfg = {.ops = lops,
                            .api = &apiops,
                            .db = &dbops,
                            .settings = settings,
                            .wall = &wall,
                            .random = &rnd,
                            .on_welcome = fake_on_welcome};
    captcha_t *cap = captcha_create(&cfg);

    tg_chat_t chat = {.id = -100};
    (void)snprintf(chat.type, sizeof(chat.type), "supergroup");
    tg_user_t user = {.id = 300};
    assert_int_equal(captcha_begin(cap, &chat, &user), 0);
    /* Restricted first, then the inline prompt; no welcome yet. */
    assert_int_equal(api.restricts, 1);
    assert_int_equal(api.send_messages, 1);
    assert_int_equal(db.captcha_put, 1);
    assert_int_equal(g_welcomed, 0);
    /* The prompt callback data carries the cap: prefix. */
    assert_non_null(strstr(api.last_body, "cap:-100:300:"));
    captcha_destroy(cap);
    settings_destroy(settings);
}

static void test_join_welcomes_when_disabled(void **state)
{
    (void)state;
    g_welcomed = 0;
    fake_db_t db = {0};
    db.settings.captcha_enabled = 0;
    db_ops_t dbops = {.submit = fake_submit, .ctx = &db};
    fake_api_t api = {0};
    tg_api_ops_t apiops = {.request = fake_request, .ctx = &api};
    int64_t now = 1700000000;
    wall_clock_ops_t wall = {.now_unix = fake_now_unix, .ctx = &now};
    random_ops_t rnd = {.next_u64 = fake_next_u64, .ctx = NULL};
    fake_ring_t ring;
    const loop_ops_t *lops = fake_ring_init(&ring);
    settings_config_t scfg = {.db = &dbops, .clock = &util_clock_ops_real};
    settings_t *settings = settings_create(&scfg);
    captcha_config_t cfg = {.ops = lops,
                            .api = &apiops,
                            .db = &dbops,
                            .settings = settings,
                            .wall = &wall,
                            .random = &rnd,
                            .on_welcome = fake_on_welcome};
    captcha_t *cap = captcha_create(&cfg);

    tg_chat_t chat = {.id = -100};
    (void)snprintf(chat.type, sizeof(chat.type), "supergroup");
    tg_user_t user = {.id = 300};
    assert_int_equal(captcha_begin(cap, &chat, &user), 0);
    assert_int_equal(api.restricts, 0); /* no challenge */
    assert_int_equal(api.send_messages, 0);
    assert_int_equal(g_welcomed, 1); /* greeted immediately */
    captcha_destroy(cap);
    settings_destroy(settings);
}

static void test_bad_args(void **state)
{
    (void)state;
    assert_null(captcha_create(NULL));
    assert_int_equal(captcha_sweep_now(NULL), -EINVAL);
    captcha_stop(NULL);    /* no-op */
    captcha_destroy(NULL); /* no-op */
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_parse_callback_ok),
        cmocka_unit_test(test_parse_callback_rejects),
        cmocka_unit_test(test_callback_wrong_user_rejected),
        cmocka_unit_test(test_callback_correct_user_lifts),
        cmocka_unit_test(test_callback_nonce_mismatch_rejected),
        cmocka_unit_test(test_sweep_kicks_due_members),
        cmocka_unit_test(test_sweep_empty),
        cmocka_unit_test(test_join_challenges_when_enabled),
        cmocka_unit_test(test_join_welcomes_when_disabled),
        cmocka_unit_test(test_bad_args),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
