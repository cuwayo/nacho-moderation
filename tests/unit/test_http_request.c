/* test_http_request.c - inbound HTTP request parser and response builder (A5.1).
 *
 * Covers the webhook-mode request parser: request line, method recognition,
 * header case-insensitivity, Content-Length bodies split at arbitrary byte
 * boundaries, the secret-token header, rejection of chunked bodies, oversize
 * rejection, and the response builder.
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

static int feed_all(http_req_t *r, const char *buf, size_t len)
{
    size_t consumed = 0;
    return http_req_feed(r, buf, len, &consumed);
}

static int feed_bytewise(http_req_t *r, const char *buf, size_t len)
{
    int rc = 0;
    for (size_t i = 0; i < len; ++i) {
        size_t consumed = 0;
        rc = http_req_feed(r, buf + i, 1, &consumed);
        if (rc != 0) {
            return rc;
        }
    }
    return rc;
}

static void test_post_with_body(void **state)
{
    (void)state;
    http_req_t r;
    assert_int_equal(http_req_init(&r, 0), 0);
    const char *req = "POST /webhook HTTP/1.1\r\nHost: x\r\n"
                      "X-Telegram-Bot-Api-Secret-Token: s3cr3t\r\n"
                      "Content-Length: 10\r\n\r\n{\"a\":true}";
    assert_int_equal(feed_all(&r, req, strlen(req)), 1);
    assert_int_equal(r.method, HTTP_METHOD_POST);
    assert_string_equal(r.target, "/webhook");
    assert_string_equal(r.secret, "s3cr3t");
    assert_int_equal(r.body_len, 10);
    assert_memory_equal(r.body, "{\"a\":true}", 10);
    http_req_free(&r);
}

static void test_get_has_no_body(void **state)
{
    (void)state;
    http_req_t r;
    assert_int_equal(http_req_init(&r, 0), 0);
    const char *req = "GET /health HTTP/1.1\r\nHost: x\r\n\r\n";
    assert_int_equal(feed_all(&r, req, strlen(req)), 1);
    assert_int_equal(r.method, HTTP_METHOD_GET);
    assert_string_equal(r.target, "/health");
    assert_int_equal(r.body_len, 0);
    assert_string_equal(r.secret, "");
    http_req_free(&r);
}

static void test_unknown_method(void **state)
{
    (void)state;
    http_req_t r;
    assert_int_equal(http_req_init(&r, 0), 0);
    const char *req = "DELETE /x HTTP/1.1\r\n\r\n";
    assert_int_equal(feed_all(&r, req, strlen(req)), 1);
    assert_int_equal(r.method, HTTP_METHOD_UNKNOWN);
    http_req_free(&r);
}

static void test_header_case_insensitive_and_body_bytewise(void **state)
{
    (void)state;
    http_req_t r;
    assert_int_equal(http_req_init(&r, 0), 0);
    const char *req = "POST /w HTTP/1.1\r\ncontent-length: 5\r\n\r\nhello";
    assert_int_equal(feed_bytewise(&r, req, strlen(req)), 1);
    assert_int_equal(r.body_len, 5);
    assert_memory_equal(r.body, "hello", 5);
    http_req_free(&r);
}

static void test_chunked_body_rejected(void **state)
{
    (void)state;
    http_req_t r;
    assert_int_equal(http_req_init(&r, 0), 0);
    const char *req = "POST /w HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
    assert_int_equal(feed_all(&r, req, strlen(req)), -EPROTO);
    assert_int_equal(r.state, HRQ_ERROR);
    http_req_free(&r);
}

static void test_oversize_body_rejected(void **state)
{
    (void)state;
    http_req_t r;
    assert_int_equal(http_req_init(&r, 4), 0);
    const char *req = "POST /w HTTP/1.1\r\nContent-Length: 10\r\n\r\n0123456789";
    assert_int_equal(feed_all(&r, req, strlen(req)), -EPROTO);
    assert_int_equal(r.state, HRQ_ERROR);
    http_req_free(&r);
}

static void test_malformed_request_line_rejected(void **state)
{
    (void)state;
    http_req_t r;
    assert_int_equal(http_req_init(&r, 0), 0);
    const char *req = "GARBAGE\r\n\r\n";
    assert_int_equal(feed_all(&r, req, strlen(req)), -EPROTO);
    http_req_free(&r);
}

/* Regression: a malformed request line must be reported on the byte that
 * completes it, not deferred to the next byte (which may never arrive). */
static void test_malformed_line_rejected_immediately(void **state)
{
    (void)state;
    http_req_t r;
    assert_int_equal(http_req_init(&r, 0), 0);
    /* Only the malformed request line, nothing after it. */
    const char *req = "GARBAGE\r\n";
    assert_int_equal(feed_all(&r, req, strlen(req)), -EPROTO);
    assert_int_equal(r.state, HRQ_ERROR);
    http_req_free(&r);
}

static void test_header_without_colon_rejected_immediately(void **state)
{
    (void)state;
    http_req_t r;
    assert_int_equal(http_req_init(&r, 0), 0);
    /* A header line with no colon, ending the feed: rejected on that line. */
    const char *req = "POST /w HTTP/1.1\r\nbroken\r\n";
    assert_int_equal(feed_all(&r, req, strlen(req)), -EPROTO);
    assert_int_equal(r.state, HRQ_ERROR);
    http_req_free(&r);
}

static void test_partial_then_rest(void **state)
{
    (void)state;
    http_req_t r;
    assert_int_equal(http_req_init(&r, 0), 0);
    const char *head = "POST /w HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello ";
    const char *tail = "world";
    assert_int_equal(feed_all(&r, head, strlen(head)), 0);
    assert_int_equal(feed_all(&r, tail, strlen(tail)), 1);
    assert_int_equal(r.body_len, 11);
    assert_memory_equal(r.body, "hello world", 11);
    http_req_free(&r);
}

static void test_response_build(void **state)
{
    (void)state;
    char out[256];
    size_t n = 0;
    const char *body = "{\"ok\":true}";
    assert_int_equal(http_response_build(out, sizeof(out), 200, body, strlen(body), &n), 0);
    assert_true(strncmp(out, "HTTP/1.1 200 OK\r\n", 17) == 0);
    assert_non_null(strstr(out, "Content-Length: 11\r\n"));
    assert_non_null(strstr(out, "Connection: close\r\n"));
    assert_non_null(strstr(out, "\r\n\r\n{\"ok\":true}"));
    assert_int_equal(n, strlen(out));

    assert_int_equal(http_response_build(out, sizeof(out), 403, body, strlen(body), &n), 0);
    assert_non_null(strstr(out, "HTTP/1.1 403 Forbidden\r\n"));
    assert_int_equal(http_response_build(out, sizeof(out), 400, NULL, 0, &n), 0);
    assert_non_null(strstr(out, "HTTP/1.1 400 Bad Request\r\n"));
    assert_int_equal(http_response_build(out, sizeof(out), 404, NULL, 0, &n), 0);
    assert_non_null(strstr(out, "HTTP/1.1 404 Not Found\r\n"));
    assert_int_equal(http_response_build(out, sizeof(out), 405, NULL, 0, &n), 0);
    assert_non_null(strstr(out, "HTTP/1.1 405 Method Not Allowed\r\n"));
    assert_int_equal(http_response_build(out, sizeof(out), 500, NULL, 0, &n), 0);
    assert_non_null(strstr(out, "HTTP/1.1 500 Error\r\n"));

    char small[8];
    assert_int_equal(http_response_build(small, sizeof(small), 200, body, strlen(body), &n),
                     -ENOSPC);
    assert_int_equal(http_response_build(NULL, 0, 200, body, strlen(body), &n), -EINVAL);
    assert_int_equal(http_response_build(out, sizeof(out), 200, NULL, 3, &n), -EINVAL);
}

static void test_header_without_colon_rejected(void **state)
{
    (void)state;
    http_req_t r;
    assert_int_equal(http_req_init(&r, 0), 0);
    const char *req = "POST /w HTTP/1.1\r\nbroken header line\r\n\r\n";
    assert_int_equal(feed_all(&r, req, strlen(req)), -EPROTO);
    assert_int_equal(r.state, HRQ_ERROR);
    http_req_free(&r);
}

static void test_oversize_header_rejected(void **state)
{
    (void)state;
    http_req_t r;
    assert_int_equal(http_req_init(&r, 0), 0);
    r.max_header = 8; /* tiny cap */
    const char *req = "POST /w HTTP/1.1\r\nHost: example\r\n\r\n";
    assert_int_equal(feed_all(&r, req, strlen(req)), -EPROTO);
    assert_int_equal(r.state, HRQ_ERROR);
    http_req_free(&r);
}

static void test_feed_after_error_stays_error(void **state)
{
    (void)state;
    http_req_t r;
    assert_int_equal(http_req_init(&r, 0), 0);
    const char *bad = "GARBAGE\r\n\r\n";
    assert_int_equal(feed_all(&r, bad, strlen(bad)), -EPROTO);
    /* Feeding again must not crash and must keep reporting the error. */
    assert_int_equal(feed_all(&r, "more", 4), -EPROTO);
    http_req_free(&r);
}

static void test_content_length_zero_no_body(void **state)
{
    (void)state;
    http_req_t r;
    assert_int_equal(http_req_init(&r, 0), 0);
    const char *req = "POST /w HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    assert_int_equal(feed_all(&r, req, strlen(req)), 1);
    assert_int_equal(r.body_len, 0);
    http_req_free(&r);
}

static void test_http_req_init_rejects_null(void **state)
{
    (void)state;
    assert_int_equal(http_req_init(NULL, 0), -EINVAL);
    assert_int_equal(http_req_feed(NULL, "x", 1, NULL), -EINVAL);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_post_with_body),
        cmocka_unit_test(test_get_has_no_body),
        cmocka_unit_test(test_unknown_method),
        cmocka_unit_test(test_header_case_insensitive_and_body_bytewise),
        cmocka_unit_test(test_chunked_body_rejected),
        cmocka_unit_test(test_oversize_body_rejected),
        cmocka_unit_test(test_malformed_request_line_rejected),
        cmocka_unit_test(test_malformed_line_rejected_immediately),
        cmocka_unit_test(test_header_without_colon_rejected_immediately),
        cmocka_unit_test(test_partial_then_rest),
        cmocka_unit_test(test_response_build),
        cmocka_unit_test(test_header_without_colon_rejected),
        cmocka_unit_test(test_oversize_header_rejected),
        cmocka_unit_test(test_feed_after_error_stays_error),
        cmocka_unit_test(test_content_length_zero_no_body),
        cmocka_unit_test(test_http_req_init_rejects_null),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
