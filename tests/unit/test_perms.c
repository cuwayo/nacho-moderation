/* test_perms.c - permission matrix and admin cache (SPEC-ADDENDUM-TOOLCHAIN A5.1).
 *
 * The matrix is pure; the admin-cache path is driven with a fake tg_api_ops_t so
 * no network is needed (cache hit vs. fetch vs. failure, and invalidation).
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
#include "tg/errors.h"
#include "tg/perms.h"
#include "tg/tg_api.h"
#include "tg/util.h"

static uint64_t fake_now_ms(void *ctx)
{
    return *(uint64_t *)ctx;
}

/* --- Pure matrix ----------------------------------------------------------- */

static const cmd_desc_t cmd_admin_restrict = {.id = CMD_COUNT,
                                              .name = "ban",
                                              .min_role = ROLE_ADMIN,
                                              .user_right = RIGHT_RESTRICT,
                                              .bot_right = RIGHT_RESTRICT,
                                              .flags = 0,
                                              .usage = "",
                                              .brief = "",
                                              .handler = NULL};
static const cmd_desc_t cmd_anyone = {.id = CMD_COUNT,
                                      .name = "help",
                                      .min_role = ROLE_ANYONE,
                                      .user_right = RIGHT_NONE,
                                      .bot_right = RIGHT_NONE,
                                      .flags = 0,
                                      .usage = "",
                                      .brief = "",
                                      .handler = NULL};

static void test_member_rejected_for_admin_command(void **state)
{
    (void)state;
    perms_subject_t member = {.role = ROLE_ANYONE, .rights = 0, .is_sudo = 0};
    assert_int_equal(perms_check(&member, &cmd_admin_restrict, 1), -ERR_PERM);
}

static void test_member_allowed_for_anyone_command(void **state)
{
    (void)state;
    perms_subject_t member = {.role = ROLE_ANYONE, .rights = 0, .is_sudo = 0};
    assert_int_equal(perms_check(&member, &cmd_anyone, 1), 0);
}

static void test_admin_with_right_allowed(void **state)
{
    (void)state;
    perms_subject_t admin = {.role = ROLE_ADMIN, .rights = RIGHT_BIT(RIGHT_RESTRICT), .is_sudo = 0};
    assert_int_equal(perms_check(&admin, &cmd_admin_restrict, 1), 0);
}

static void test_admin_without_right_rejected(void **state)
{
    (void)state;
    perms_subject_t admin = {.role = ROLE_ADMIN, .rights = RIGHT_BIT(RIGHT_DELETE), .is_sudo = 0};
    assert_int_equal(perms_check(&admin, &cmd_admin_restrict, 1), -ERR_PERM);
}

static void test_owner_bypasses_right(void **state)
{
    (void)state;
    perms_subject_t owner = {.role = ROLE_OWNER, .rights = 0, .is_sudo = 0};
    assert_int_equal(perms_check(&owner, &cmd_admin_restrict, 1), 0);
}

static void test_sudo_bypasses_role_and_right(void **state)
{
    (void)state;
    perms_subject_t sudo = {.role = ROLE_ANYONE, .rights = 0, .is_sudo = 1};
    assert_int_equal(perms_check(&sudo, &cmd_admin_restrict, 1), 0);
}

static void test_bot_lacking_right_reports_api_error(void **state)
{
    (void)state;
    perms_subject_t owner = {.role = ROLE_OWNER, .rights = 0, .is_sudo = 0};
    /* Even the owner cannot run the command if the BOT lacks the right. */
    assert_int_equal(perms_check(&owner, &cmd_admin_restrict, 0), -ERR_TG_API);
}

static void test_target_rules(void **state)
{
    (void)state;
    const int64_t bot_id = 999;
    assert_int_equal(perms_target_ok(ROLE_ANYONE, 5, bot_id), 0);              /* plain member ok */
    assert_int_equal(perms_target_ok(ROLE_ANYONE, bot_id, bot_id), -ERR_PERM); /* bot itself */
    assert_int_equal(perms_target_ok(ROLE_ADMIN, 5, bot_id), -ERR_PERM);       /* admin */
    assert_int_equal(perms_target_ok(ROLE_OWNER, 5, bot_id), -ERR_PERM);       /* owner */
}

static void test_null_command_allowed(void **state)
{
    (void)state;
    perms_subject_t member = {.role = ROLE_ANYONE, .rights = 0, .is_sudo = 0};
    assert_int_equal(perms_check(&member, NULL, 1), 0);
    assert_int_equal(perms_check(NULL, &cmd_anyone, 1), -EINVAL);
}

static const cmd_desc_t cmd_owner_only = {.id = CMD_COUNT,
                                          .name = "setlog",
                                          .min_role = ROLE_OWNER,
                                          .user_right = RIGHT_NONE,
                                          .bot_right = RIGHT_NONE,
                                          .flags = 0,
                                          .usage = "",
                                          .brief = "",
                                          .handler = NULL};
static const cmd_desc_t cmd_sudo_only = {.id = CMD_COUNT,
                                         .name = "sudo",
                                         .min_role = ROLE_SUDO,
                                         .user_right = RIGHT_NONE,
                                         .bot_right = RIGHT_NONE,
                                         .flags = 0,
                                         .usage = "",
                                         .brief = "",
                                         .handler = NULL};

static void test_owner_and_sudo_min_role(void **state)
{
    (void)state;
    perms_subject_t admin = {.role = ROLE_ADMIN, .rights = 0xffffffffu, .is_sudo = 0};
    /* An administrator cannot run an owner-only command. */
    assert_int_equal(perms_check(&admin, &cmd_owner_only, 1), -ERR_PERM);
    /* A non-sudo member cannot run a sudo-only command. */
    perms_subject_t member = {.role = ROLE_ANYONE, .rights = 0, .is_sudo = 0};
    assert_int_equal(perms_check(&member, &cmd_sudo_only, 1), -ERR_PERM);
    /* A sudo user can. */
    perms_subject_t sudo = {.role = ROLE_ANYONE, .rights = 0, .is_sudo = 1};
    assert_int_equal(perms_check(&sudo, &cmd_sudo_only, 1), 0);
    /* The owner can run the owner-only command. */
    perms_subject_t owner = {.role = ROLE_OWNER, .rights = 0, .is_sudo = 0};
    assert_int_equal(perms_check(&owner, &cmd_owner_only, 1), 0);
}

/* --- Admin cache over a fake API seam -------------------------------------- */

typedef struct {
    int requests;
    method_id_t last_method;
    api_result_t result; /**< Delivered on the next fake_deliver(). */
    tg_api_cb pending_cb;
    void *pending_ctx;
} fake_api_t;

static int fake_request(void *ctx, method_id_t method, const char *body, size_t body_len,
                        int64_t chat_id, uint64_t deadline_ms, tg_api_cb cb, void *user)
{
    (void)body;
    (void)body_len;
    (void)chat_id;
    (void)deadline_ms;
    fake_api_t *f = ctx;
    f->requests++;
    f->last_method = method;
    f->pending_cb = cb;
    f->pending_ctx = user;
    return 0;
}

static void fake_deliver(fake_api_t *f)
{
    tg_api_cb cb = f->pending_cb;
    f->pending_cb = NULL;
    if (cb != NULL) {
        cb(&f->result, f->pending_ctx);
    }
}

typedef struct {
    int calls;
    int status;
    role_t role;
    unsigned rights;
    int is_sudo;
} subject_capture_t;

static void on_subject(const perms_subject_t *subject, int status, void *ctx)
{
    subject_capture_t *cap = ctx;
    cap->calls++;
    cap->status = status;
    if (subject != NULL) {
        cap->role = subject->role;
        cap->rights = subject->rights;
        cap->is_sudo = subject->is_sudo;
    }
}

/** @brief Copy an admin list into a caller-provided buffer. */
static void capture_admins(const perms_admins_t *admins, int status, void *ctx)
{
    perms_admins_t *out = ctx;
    if (status == 0 && admins != NULL) {
        *out = *admins;
    }
}

static const char *ADMINS_JSON =
    "{\"ok\":true,\"result\":["
    "{\"status\":\"creator\",\"user\":{\"id\":1,\"is_bot\":false,\"first_name\":\"Own\","
    "\"username\":\"owner\"}},"
    "{\"status\":\"administrator\",\"user\":{\"id\":2,\"first_name\":\"Adm\","
    "\"username\":\"admin\"},"
    "\"can_restrict_members\":true,\"can_delete_messages\":false},"
    "{\"status\":\"administrator\",\"user\":{\"id\":3,\"first_name\":\"NoRights\"}}]}";

static void test_admin_fetch_captures_username(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    fake_api_t f = {0};
    tg_api_ops_t ops = {.request = fake_request, .ctx = &f};
    perms_config_t cfg = {.clock = &clock, .api = &ops, .bot_id = 999};
    perms_t *p = perms_create(&cfg);
    assert_non_null(p);

    /* A getChatAdministrators result carries each admin's username and name; the
     * router seeds the users cache from these so @username targets resolve. */
    perms_admins_t captured = {0};
    assert_int_equal(perms_admins_get(p, -100, capture_admins, &captured), 0);
    f.result = (api_result_t){.status = 0, .body = ADMINS_JSON, .body_len = strlen(ADMINS_JSON)};
    fake_deliver(&f);
    assert_int_equal(captured.count, 3);
    assert_string_equal(captured.admins[0].username, "owner");
    assert_string_equal(captured.admins[0].first_name, "Own");
    assert_string_equal(captured.admins[1].username, "admin");
    assert_string_equal(captured.admins[2].username, ""); /* no username in the fixture */

    perms_destroy(p);
}

static void test_admin_fetch_then_cache_hit(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    fake_api_t f = {0};
    tg_api_ops_t ops = {.request = fake_request, .ctx = &f};
    perms_config_t cfg = {.clock = &clock, .api = &ops, .bot_id = 999};
    perms_t *p = perms_create(&cfg);
    assert_non_null(p);

    /* Miss: a getChatAdministrators request is issued. */
    subject_capture_t cap = {0};
    assert_int_equal(perms_subject_get(p, -100, 2, on_subject, &cap), 0);
    assert_int_equal(f.requests, 1);
    assert_int_equal(f.last_method, METHOD_GET_CHAT_ADMINISTRATORS);
    assert_int_equal(cap.calls, 0); /* pending */

    f.result = (api_result_t){.status = 0, .body = ADMINS_JSON, .body_len = strlen(ADMINS_JSON)};
    fake_deliver(&f);
    assert_int_equal(cap.calls, 1);
    assert_int_equal(cap.status, 0);
    assert_int_equal(cap.role, ROLE_ADMIN);
    assert_true((cap.rights & RIGHT_BIT(RIGHT_RESTRICT)) != 0u);
    assert_false((cap.rights & RIGHT_BIT(RIGHT_DELETE)) != 0u);

    /* Hit: no new request, resolved synchronously. */
    subject_capture_t cap2 = {0};
    assert_int_equal(perms_subject_get(p, -100, 1, on_subject, &cap2), 0);
    assert_int_equal(f.requests, 1); /* unchanged */
    assert_int_equal(cap2.calls, 1);
    assert_int_equal(cap2.role, ROLE_OWNER);

    perms_destroy(p);
}

static void test_invalidate_forces_refetch(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    fake_api_t f = {0};
    tg_api_ops_t ops = {.request = fake_request, .ctx = &f};
    perms_config_t cfg = {.clock = &clock, .api = &ops, .bot_id = 999};
    perms_t *p = perms_create(&cfg);

    subject_capture_t cap = {0};
    (void)perms_subject_get(p, -100, 2, on_subject, &cap);
    f.result = (api_result_t){.status = 0, .body = ADMINS_JSON, .body_len = strlen(ADMINS_JSON)};
    fake_deliver(&f);
    assert_int_equal(f.requests, 1);

    perms_invalidate(p, -100);
    subject_capture_t cap2 = {0};
    (void)perms_subject_get(p, -100, 2, on_subject, &cap2);
    assert_int_equal(f.requests, 2); /* refetched after invalidation */
    /* Deliver the refetched reply so the in-flight context is released. */
    f.result = (api_result_t){.status = 0, .body = ADMINS_JSON, .body_len = strlen(ADMINS_JSON)};
    fake_deliver(&f);
    assert_int_equal(cap2.calls, 1);
    perms_destroy(p);
}

static void test_fetch_failure_reports_status(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    fake_api_t f = {0};
    tg_api_ops_t ops = {.request = fake_request, .ctx = &f};
    perms_config_t cfg = {.clock = &clock, .api = &ops, .bot_id = 999};
    perms_t *p = perms_create(&cfg);

    subject_capture_t cap = {0};
    (void)perms_subject_get(p, -100, 2, on_subject, &cap);
    f.result = (api_result_t){.status = -ERR_NET};
    fake_deliver(&f);
    assert_int_equal(cap.calls, 1);
    assert_int_equal(cap.status, -ERR_NET);
    perms_destroy(p);
}

static void test_sudo_classification(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    fake_api_t f = {0};
    tg_api_ops_t ops = {.request = fake_request, .ctx = &f};
    const int64_t sudo[] = {555};
    perms_config_t cfg = {
        .clock = &clock, .api = &ops, .bot_id = 999, .sudo_ids = sudo, .sudo_count = 1};
    perms_t *p = perms_create(&cfg);
    assert_true(perms_is_sudo(p, 555));
    assert_false(perms_is_sudo(p, 556));

    /* A sudo user is flagged even though absent from the admin list. */
    perms_subject_t subject;
    perms_admins_t admins = {0};
    assert_int_equal(perms_subject_from_admins(p, &admins, 555, &subject), 0);
    assert_true(subject.is_sudo);
    perms_destroy(p);
}

static void test_bot_right_and_role_lookup(void **state)
{
    (void)state;
    const int64_t bot_id = 999;
    perms_admins_t admins = {0};
    admins.count = 2;
    admins.admins[0] = (perms_admin_t){.user_id = 100, .role = ROLE_OWNER, .rights = 0xffffffffu};
    admins.admins[1] =
        (perms_admin_t){.user_id = 200, .role = ROLE_ADMIN, .rights = RIGHT_BIT(RIGHT_RESTRICT)};

    /* RIGHT_NONE is always satisfied, even with no admin list. */
    assert_int_equal(perms_bot_has_right(NULL, bot_id, RIGHT_NONE), 1);
    /* The bot is not in the list: it holds no right. */
    assert_int_equal(perms_bot_has_right(&admins, bot_id, RIGHT_RESTRICT), 0);

    /* Role lookup against the original list. */
    assert_int_equal(perms_role_of(&admins, 100), ROLE_OWNER);
    assert_int_equal(perms_role_of(&admins, 200), ROLE_ADMIN);
    assert_int_equal(perms_role_of(&admins, 555), ROLE_ANYONE);
    assert_int_equal(perms_role_of(NULL, 555), ROLE_ANYONE);

    /* The bot as a plain administrator holds only its granted rights. */
    admins.admins[1].user_id = bot_id;
    assert_int_equal(perms_bot_has_right(&admins, bot_id, RIGHT_RESTRICT), 1);
    assert_int_equal(perms_bot_has_right(&admins, bot_id, RIGHT_DELETE), 0);

    /* The bot as the creator holds every right. */
    admins.admins[0].user_id = bot_id;
    admins.admins[0].role = ROLE_OWNER;
    assert_int_equal(perms_bot_has_right(&admins, bot_id, RIGHT_DELETE), 1);
    assert_int_equal(perms_role_of(&admins, bot_id), ROLE_OWNER);
}

static void test_bad_args(void **state)
{
    (void)state;
    assert_null(perms_create(NULL));
    perms_destroy(NULL); /* no-op */
    perms_invalidate(NULL, 1);
    assert_int_equal(perms_is_sudo(NULL, 1), 0);
}

/* A request that fails to queue must not run the callback. */
static int fail_request(void *ctx, method_id_t method, const char *body, size_t body_len,
                        int64_t chat_id, uint64_t deadline_ms, tg_api_cb cb, void *user)
{
    (void)ctx;
    (void)method;
    (void)body;
    (void)body_len;
    (void)chat_id;
    (void)deadline_ms;
    (void)cb;
    (void)user;
    return -ENOMEM;
}

static void test_request_failure_does_not_callback(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    tg_api_ops_t ops = {.request = fail_request, .ctx = NULL};
    perms_config_t cfg = {.clock = &clock, .api = &ops, .bot_id = 999};
    perms_t *p = perms_create(&cfg);

    subject_capture_t cap = {0};
    assert_int_equal(perms_subject_get(p, -100, 2, on_subject, &cap), -ENOMEM);
    assert_int_equal(cap.calls, 0);
    assert_int_equal(perms_admins_get(p, -100, NULL, NULL), -EINVAL);
    perms_destroy(p);
}

/* An admin list with malformed entries must be tolerated. */
static void test_malformed_admin_entries_skipped(void **state)
{
    (void)state;
    uint64_t now = 0;
    clock_ops_t clock = {.now_ms = fake_now_ms, .ctx = &now};
    fake_api_t f = {0};
    tg_api_ops_t ops = {.request = fake_request, .ctx = &f};
    perms_config_t cfg = {.clock = &clock, .api = &ops, .bot_id = 999};
    perms_t *p = perms_create(&cfg);

    subject_capture_t cap = {0};
    (void)perms_subject_get(p, -100, 2, on_subject, &cap);
    const char *bad = "{\"ok\":true,\"result\":[{\"status\":\"administrator\"},"
                      "{\"user\":{\"id\":\"not-int\"}},"
                      "{\"user\":{\"id\":2,\"first_name\":\"Ok\"},\"status\":\"administrator\"}]}";
    f.result = (api_result_t){.status = 0, .body = bad, .body_len = strlen(bad)};
    fake_deliver(&f);
    assert_int_equal(cap.calls, 1);
    assert_int_equal(cap.status, 0);
    assert_int_equal(cap.role, ROLE_ADMIN);

    /* A malformed envelope fails the fetch. */
    subject_capture_t cap2 = {0};
    (void)perms_subject_get(p, -200, 2, on_subject, &cap2);
    const char *garbage = "not json";
    f.result = (api_result_t){.status = 0, .body = garbage, .body_len = strlen(garbage)};
    fake_deliver(&f);
    assert_int_equal(cap2.calls, 1);
    assert_int_equal(cap2.status, -ERR_PARSE);
    perms_destroy(p);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_member_rejected_for_admin_command),
        cmocka_unit_test(test_member_allowed_for_anyone_command),
        cmocka_unit_test(test_admin_with_right_allowed),
        cmocka_unit_test(test_admin_without_right_rejected),
        cmocka_unit_test(test_owner_bypasses_right),
        cmocka_unit_test(test_sudo_bypasses_role_and_right),
        cmocka_unit_test(test_bot_lacking_right_reports_api_error),
        cmocka_unit_test(test_target_rules),
        cmocka_unit_test(test_null_command_allowed),
        cmocka_unit_test(test_owner_and_sudo_min_role),
        cmocka_unit_test(test_admin_fetch_captures_username),
        cmocka_unit_test(test_admin_fetch_then_cache_hit),
        cmocka_unit_test(test_invalidate_forces_refetch),
        cmocka_unit_test(test_fetch_failure_reports_status),
        cmocka_unit_test(test_sudo_classification),
        cmocka_unit_test(test_request_failure_does_not_callback),
        cmocka_unit_test(test_malformed_admin_entries_skipped),
        cmocka_unit_test(test_bot_right_and_role_lookup),
        cmocka_unit_test(test_bad_args),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
