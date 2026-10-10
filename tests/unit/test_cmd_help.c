/* test_cmd_help.c - the /help handler and the rendered command list (S2, A5.1).
 *
 * /help is rendered entirely from cmd_table; the test drives it through a fake
 * reply hook so no network is involved, and checks the standalone renderers.
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

#include "commands.h"
#include "tables.h"
#include "tg/cmd_ctx.h"
#include "tg/tg_types.h"
#include "tg/util.h"

/* --- Reply capture --------------------------------------------------------- */

typedef struct {
    int calls;
    char last[2048];
    int64_t reply_to;
} reply_capture_t;

static int capture_reply(struct cmd_ctx *ctx, const char *html, int64_t reply_to)
{
    reply_capture_t *cap = ctx->reply_ctx;
    cap->calls++;
    (void)snprintf(cap->last, sizeof(cap->last), "%s", html);
    cap->reply_to = reply_to;
    return 0;
}

static void test_help_renders_and_replies(void **state)
{
    (void)state;
    reply_capture_t cap = {0};
    tg_message_t msg = {.message_id = 7};
    struct cmd_ctx ctx = {.cmd = cmd_lookup("help"),
                          .chat_id = -100,
                          .user_id = 1,
                          .args = "",
                          .msg = &msg,
                          .is_group = 1,
                          .role = ROLE_ANYONE,
                          .reply = capture_reply,
                          .reply_ctx = &cap};
    assert_int_equal(cmd_help(&ctx), 0);
    assert_int_equal(cap.calls, 1);
    assert_int_equal(cap.reply_to, 7);
    assert_non_null(strstr(cap.last, "/help"));
}

static void test_help_rejects_null(void **state)
{
    (void)state;
    assert_int_equal(cmd_help(NULL), -EINVAL);
    struct cmd_ctx ctx = {0};
    assert_int_equal(cmd_help(&ctx), -EINVAL); /* no reply hook */
}

/* --- Renderers ------------------------------------------------------------- */

static void test_render_help_anyone_in_group(void **state)
{
    (void)state;
    strbuf_t b;
    assert_int_equal(strbuf_init(&b, 128), 0);
    assert_int_equal(commands_render_help(ROLE_ANYONE, 1, &b), 0);
    assert_non_null(strstr(strbuf_cstr(&b), "/help"));
    strbuf_free(&b);
}

static void test_render_help_private_hides_group_only(void **state)
{
    (void)state;
    /* No group-only commands exist yet, so the private list must still contain
     * /help and must not crash on an empty group-only set. */
    strbuf_t b;
    assert_int_equal(strbuf_init(&b, 128), 0);
    assert_int_equal(commands_render_help(ROLE_ANYONE, 0, &b), 0);
    assert_non_null(strstr(strbuf_cstr(&b), "/help"));
    strbuf_free(&b);

    strbuf_t b2;
    assert_int_equal(strbuf_init(&b2, 8), 0);
    assert_int_equal(commands_render_help(ROLE_OWNER, 1, NULL), -EINVAL);
    strbuf_free(&b2);
}

static void test_set_my_commands_payload(void **state)
{
    (void)state;
    char buf[4096];
    size_t len = 0;
    assert_int_equal(commands_build_set_my_commands(buf, sizeof(buf), &len), 0);
    assert_int_equal(len, strlen(buf));
    assert_non_null(strstr(buf, "\"commands\":["));
    assert_non_null(strstr(buf, "\"command\":\"help\""));
    assert_non_null(strstr(buf, "\"description\":\"Show available commands\""));
    assert_int_equal(buf[len - 1], '}');

    char small[8];
    assert_int_equal(commands_build_set_my_commands(small, sizeof(small), &len), -ENOSPC);
    assert_int_equal(commands_build_set_my_commands(NULL, 8, &len), -EINVAL);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_help_renders_and_replies),
        cmocka_unit_test(test_help_rejects_null),
        cmocka_unit_test(test_render_help_anyone_in_group),
        cmocka_unit_test(test_render_help_private_hides_group_only),
        cmocka_unit_test(test_set_my_commands_payload),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
