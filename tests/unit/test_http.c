/* test_http.c - incremental HTTP/1.1 response parser (A5.1).
 *
 * Covers status line, case-insensitive headers, Content-Length, chunked bodies
 * split at every byte boundary, keep-alive vs Connection: close, oversize
 * rejection, and one-byte-at-a-time feeding.
 */
// NOLINTBEGIN(misc-include-cleaner)
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
// NOLINTEND(misc-include-cleaner)

#include <errno.h>
#include <string.h>

#include <cmocka.h>

#include "tg/http.h"

/* Feed a whole buffer in one call and return the final result. */
static int feed_all(http_resp_t *r, const char *buf, size_t len)
{
    size_t consumed = 0;
    return http_resp_feed(r, buf, len, &consumed);
}

/* Feed one byte at a time; returns the result of the last byte. */
static int feed_bytewise(http_resp_t *r, const char *buf, size_t len)
{
    int rc = 0;
    for (size_t i = 0; i < len; ++i) {
        size_t consumed = 0;
        rc = http_resp_feed(r, buf + i, 1, &consumed);
        if (rc != 0) {
            return rc;
        }
    }
    return rc;
}

static void test_content_length_body(void **state)
{
    (void)state;
    http_resp_t r;
    assert_int_equal(http_resp_init(&r, 0), 0);
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    assert_int_equal(feed_all(&r, resp, strlen(resp)), 1);
    assert_int_equal(r.status, 200);
    assert_int_equal(r.body_len, 5);
    assert_memory_equal(r.body, "hello", 5);
    assert_true(r.keep_alive);
    http_resp_free(&r);
}

static void test_header_case_insensitive(void **state)
{
    (void)state;
    http_resp_t r;
    assert_int_equal(http_resp_init(&r, 0), 0);
    const char *resp = "HTTP/1.1 200 OK\r\nCONTENT-length: 3\r\n\r\nabc";
    assert_int_equal(feed_all(&r, resp, strlen(resp)), 1);
    assert_int_equal(r.body_len, 3);
    http_resp_free(&r);
}

static void test_chunked_body(void **state)
{
    (void)state;
    http_resp_t r;
    assert_int_equal(http_resp_init(&r, 0), 0);
    const char *resp = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                       "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
    assert_int_equal(feed_all(&r, resp, strlen(resp)), 1);
    assert_int_equal(r.body_len, 11);
    assert_memory_equal(r.body, "hello world", 11);
    http_resp_free(&r);
}

static void test_chunked_split_every_byte(void **state)
{
    (void)state;
    http_resp_t r;
    assert_int_equal(http_resp_init(&r, 0), 0);
    const char *resp = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                       "a\r\n0123456789\r\n0\r\n\r\n";
    assert_int_equal(feed_bytewise(&r, resp, strlen(resp)), 1);
    assert_int_equal(r.body_len, 10);
    assert_memory_equal(r.body, "0123456789", 10);
    http_resp_free(&r);
}

static void test_connection_close(void **state)
{
    (void)state;
    http_resp_t r;
    assert_int_equal(http_resp_init(&r, 0), 0);
    const char *resp = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nhi";
    assert_int_equal(feed_all(&r, resp, strlen(resp)), 1);
    assert_false(r.keep_alive);
    http_resp_free(&r);
}

static void test_204_no_body(void **state)
{
    (void)state;
    http_resp_t r;
    assert_int_equal(http_resp_init(&r, 0), 0);
    const char *resp = "HTTP/1.1 204 No Content\r\n\r\n";
    assert_int_equal(feed_all(&r, resp, strlen(resp)), 1);
    assert_int_equal(r.status, 204);
    assert_int_equal(r.body_len, 0);
    http_resp_free(&r);
}

static void test_oversize_body_rejected(void **state)
{
    (void)state;
    http_resp_t r;
    assert_int_equal(http_resp_init(&r, 4), 0); /* tiny cap */
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789";
    assert_int_equal(feed_all(&r, resp, strlen(resp)), -EPROTO);
    assert_int_equal(r.state, HR_ERROR);
    http_resp_free(&r);
}

static void test_bad_status_line_rejected(void **state)
{
    (void)state;
    http_resp_t r;
    assert_int_equal(http_resp_init(&r, 0), 0);
    const char *resp = "GARBAGE\r\n\r\n";
    assert_int_equal(feed_all(&r, resp, strlen(resp)), -EPROTO);
    http_resp_free(&r);
}

static void test_partial_then_rest(void **state)
{
    (void)state;
    http_resp_t r;
    assert_int_equal(http_resp_init(&r, 0), 0);
    const char *head = "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello ";
    const char *tail = "world";
    assert_int_equal(feed_all(&r, head, strlen(head)), 0); /* not complete yet */
    assert_int_equal(feed_all(&r, tail, strlen(tail)), 1);
    assert_int_equal(r.body_len, 11);
    assert_memory_equal(r.body, "hello world", 11);
    http_resp_free(&r);
}

static void test_request_build(void **state)
{
    (void)state;
    char out[256];
    size_t n = 0;
    assert_int_equal(http_request_build(out, sizeof(out), "GET", "/botX/getMe", "api.telegram.org",
                                        "tgbot/1", &n),
                     0);
    assert_true(strstr(out, "GET /botX/getMe HTTP/1.1\r\n") == out);
    assert_non_null(strstr(out, "Host: api.telegram.org\r\n"));
    assert_non_null(strstr(out, "\r\n\r\n"));
    assert_int_equal(n, strlen(out));

    char small[8];
    assert_int_equal(http_request_build(small, sizeof(small), "GET", "/x", "h", "ua", &n), -ENOSPC);
}

static void test_post_request_build(void **state)
{
    (void)state;
    char out[256];
    size_t n = 0;
    const char *body = "{\"chat_id\":1}";
    assert_int_equal(http_request_build_post(out, sizeof(out), "/botX/sendMessage",
                                             "api.telegram.org", "tgbot/1", body, strlen(body), &n),
                     0);
    assert_true(strncmp(out, "POST /botX/sendMessage HTTP/1.1\r\n", 33) == 0);
    assert_non_null(strstr(out, "Content-Type: application/json\r\n"));
    assert_non_null(strstr(out, "Content-Length: 13\r\n"));
    assert_non_null(strstr(out, "\r\n\r\n{\"chat_id\":1}"));
    assert_int_equal(n, strlen(out));

    /* Empty body is allowed (Content-Length: 0). */
    assert_int_equal(http_request_build_post(out, sizeof(out), "/x", "h", NULL, NULL, 0, &n), 0);
    assert_non_null(strstr(out, "Content-Length: 0\r\n"));

    char small[16];
    assert_int_equal(
        http_request_build_post(small, sizeof(small), "/x", "h", "ua", body, strlen(body), &n),
        -ENOSPC);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_content_length_body),
        cmocka_unit_test(test_header_case_insensitive),
        cmocka_unit_test(test_chunked_body),
        cmocka_unit_test(test_chunked_split_every_byte),
        cmocka_unit_test(test_connection_close),
        cmocka_unit_test(test_204_no_body),
        cmocka_unit_test(test_oversize_body_rejected),
        cmocka_unit_test(test_bad_status_line_rejected),
        cmocka_unit_test(test_partial_then_rest),
        cmocka_unit_test(test_request_build),
        cmocka_unit_test(test_post_request_build),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
