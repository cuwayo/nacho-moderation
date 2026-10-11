/**
 * @file http.c
 * @brief Incremental HTTP/1.1 response parser and request builder.
 *
 * The parser is deliberately a byte-at-a-time state machine so it is correct
 * under arbitrary chunk boundaries (including one byte per feed), which the unit
 * tests and the fuzz target exercise. See http.h for the state list.
 *
 * @ingroup http
 */
#include "tg/http.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Small helpers --------------------------------------------------------- */

/**
 * @brief Case-insensitive equality for ASCII header names/values.
 *
 * @param[in] a First string.
 * @param[in] b Second string.
 * @param[in] n Bytes to compare.
 * @return 1 if equal ignoring ASCII case, else 0.
 */
static int ci_equal(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Case-insensitive substring test.
 *
 * @param[in] hay    Haystack (NUL-terminated).
 * @param[in] needle Needle (NUL-terminated).
 * @return 1 if @p needle occurs in @p hay ignoring ASCII case, else 0.
 */
static int ci_contains(const char *hay, const char *needle)
{
    size_t hn = strlen(hay);
    size_t nn = strlen(needle);
    if (nn == 0 || hn < nn) {
        return 0;
    }
    for (size_t i = 0; i + nn <= hn; ++i) {
        if (ci_equal(hay + i, needle, nn)) {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Parse a non-negative decimal integer.
 *
 * @param[in]  s   Digits (need not be NUL-terminated).
 * @param[in]  n   Number of bytes in @p s.
 * @param[out] out Receives the parsed value.
 * @return 0 on success, -EINVAL on a non-digit, -ERANGE on overflow.
 */
static int parse_u64(const char *s, size_t n, uint64_t *out)
{
    if (n == 0) {
        return -EINVAL;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return -EINVAL;
        }
        if (v > (UINT64_MAX - (uint64_t)(s[i] - '0')) / 10u) {
            return -ERANGE;
        }
        v = (v * 10u) + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

/**
 * @brief Parse a hexadecimal chunk size, stopping at any chunk extension (';').
 *
 * @param[in]  s   Hex digits (need not be NUL-terminated).
 * @param[in]  n   Number of bytes in @p s.
 * @param[out] out Receives the parsed value.
 * @return 0 on success, -EINVAL on a non-hex digit, -ERANGE on overflow.
 */
static int parse_hex(const char *s, size_t n, uint64_t *out)
{
    size_t i = 0;
    while (i < n && s[i] != ';') {
        ++i;
    }
    if (i == 0) {
        return -EINVAL;
    }
    uint64_t v = 0;
    for (size_t j = 0; j < i; ++j) {
        char c = s[j];
        uint64_t d;
        if (c >= '0' && c <= '9') {
            d = (uint64_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint64_t)(c - 'a') + 10u;
        } else if (c >= 'A' && c <= 'F') {
            d = (uint64_t)(c - 'A') + 10u;
        } else {
            return -EINVAL;
        }
        if (v > (UINT64_MAX - d) / 16u) {
            return -ERANGE;
        }
        v = (v * 16u) + d;
    }
    *out = v;
    return 0;
}

/**
 * @brief Append @p n bytes to the decoded body, enforcing the body cap.
 *
 * @param[in,out] r   Response parser.
 * @param[in]     buf Bytes to append.
 * @param[in]     n   Count.
 * @return 0 on success, -EPROTO if the body cap is exceeded, -ENOMEM on failure.
 */
static int body_append(http_resp_t *r, const char *buf, size_t n)
{
    if (r->body_len + n > r->max_body) {
        return -EPROTO;
    }
    if (r->body_len + n + 1u > r->body_cap) {
        size_t cap = r->body_cap == 0 ? 256u : r->body_cap;
        while (cap < r->body_len + n + 1u) {
            cap *= 2u;
        }
        char *nb = realloc(r->body, cap);
        if (nb == NULL) {
            return -ENOMEM;
        }
        r->body = nb;
        r->body_cap = cap;
    }
    memcpy(r->body + r->body_len, buf, n);
    r->body_len += n;
    r->body[r->body_len] = '\0';
    return 0;
}

/* --- Header line handling -------------------------------------------------- */

/**
 * @brief Parse the status line into the response status code.
 *
 * @param[in,out] r Parser (state set to HR_HEADERS or HR_ERROR).
 */
static void handle_status_line(http_resp_t *r)
{
    /* Expect "HTTP/1.x <code> ..."; take the first three digits after the space. */
    const char *sp = strchr(r->line, ' ');
    if (sp == NULL) {
        r->state = HR_ERROR;
        return;
    }
    uint64_t code = 0;
    if (parse_u64(sp + 1, 3, &code) != 0 || code < 100 || code > 599) {
        r->state = HR_ERROR;
        return;
    }
    r->status = (int)code;
    r->state = HR_HEADERS;
}

/**
 * @brief Apply a "Content-Length: N" header (ignored once chunked framing is known).
 *
 * @param[in,out] r     Parser.
 * @param[in]     value Header value.
 */
static void apply_content_length(http_resp_t *r, const char *value)
{
    uint64_t len = 0;
    if (parse_u64(value, strlen(value), &len) != 0) {
        r->state = HR_ERROR;
        return;
    }
    if (r->mode != HTTP_BODY_CHUNKED) {
        r->mode = HTTP_BODY_LENGTH;
        r->remaining = len;
    }
}

/**
 * @brief Apply one header line to the parser state.
 *
 * Recognizes Content-Length, Transfer-Encoding: chunked and Connection.
 *
 * @param[in,out] r Parser (line holds the header text).
 */
static void handle_header_line(http_resp_t *r)
{
    char *colon = strchr(r->line, ':');
    if (colon == NULL) {
        r->state = HR_ERROR;
        return;
    }
    *colon = '\0';
    const char *name = r->line;
    const char *value = colon + 1;
    while (*value == ' ' || *value == '\t') {
        ++value;
    }
    size_t nlen = strlen(name);

    if (nlen == 14 && ci_equal(name, "content-length", 14)) {
        apply_content_length(r, value);
    } else if (nlen == 17 && ci_equal(name, "transfer-encoding", 17) &&
               ci_contains(value, "chunked")) {
        r->mode = HTTP_BODY_CHUNKED;
    } else if (nlen == 10 && ci_equal(name, "connection", 10)) {
        if (ci_contains(value, "close")) {
            r->keep_alive = 0;
        } else if (ci_contains(value, "keep-alive")) {
            r->keep_alive = 1;
        }
    }
}

/**
 * @brief Decide the body framing once the header block ends.
 *
 * @param[in,out] r Parser (state advanced to the body or HR_DONE).
 */
static void finish_headers(http_resp_t *r)
{
    if (r->mode == HTTP_BODY_CHUNKED) {
        r->state = HR_BODY_CHUNK_SIZE;
    } else if (r->mode == HTTP_BODY_LENGTH && r->remaining > 0) {
        r->state = HR_BODY_LENGTH;
    } else {
        r->state = HR_DONE;
    }
}

/* --- Public API ------------------------------------------------------------ */

int http_resp_init(http_resp_t *r, size_t max_body)
{
    if (r == NULL) {
        return -EINVAL;
    }
    *r = (http_resp_t){
        .state = HR_STATUS_LINE,
        .keep_alive = 1,
        .mode = HTTP_BODY_NONE,
        .max_header = HTTP_MAX_HEADER_BYTES,
        .max_body = max_body == 0 ? HTTP_MAX_BODY_BYTES : max_body,
    };
    return 0;
}

void http_resp_free(http_resp_t *r)
{
    if (r == NULL) {
        return;
    }
    free(r->body);
    r->body = NULL;
    r->body_len = 0;
    r->body_cap = 0;
}

/**
 * @brief Act on a completed line, dispatching by the current parser state.
 *
 * @param[in,out] r Parser (line holds the completed line).
 * @return 0 to continue, 1 when the response is complete, negative errno on error.
 */
static int handle_line(http_resp_t *r)
{
    switch (r->state) {
    case HR_STATUS_LINE:
        handle_status_line(r);
        break;
    case HR_HEADERS:
        if (r->line_len == 0) {
            finish_headers(r);
        } else {
            handle_header_line(r);
        }
        break;
    case HR_BODY_CHUNK_SIZE: {
        uint64_t size = 0;
        if (parse_hex(r->line, r->line_len, &size) != 0) {
            r->state = HR_ERROR;
            return -EPROTO;
        }
        if (size == 0) {
            r->state = HR_TRAILERS;
        } else {
            r->remaining = size;
            r->state = HR_BODY_CHUNK_DATA;
        }
        break;
    }
    case HR_TRAILERS:
        if (r->line_len == 0) {
            r->state = HR_DONE;
        }
        break;
    default:
        break;
    }
    r->line_len = 0;
    if (r->state == HR_ERROR) {
        return -EPROTO;
    }
    return r->state == HR_DONE ? 1 : 0;
}

/**
 * @brief Accumulate a byte into the line buffer; act when a full line arrives.
 *
 * @param[in,out] r Parser.
 * @param[in]     c Byte to feed.
 * @return 0 to continue, 1 when complete, negative errno on error.
 */
static int feed_line_byte(http_resp_t *r, char c)
{
    if (r->line_len + 1u >= sizeof(r->line)) {
        r->state = HR_ERROR;
        return -EPROTO;
    }
    if (c != '\n') {
        r->line[r->line_len++] = c;
        return 0;
    }
    /* Strip a trailing CR. */
    if (r->line_len > 0 && r->line[r->line_len - 1] == '\r') {
        r->line[--r->line_len] = '\0';
    } else {
        r->line[r->line_len] = '\0';
    }
    return handle_line(r);
}

/**
 * @brief Process one byte; returns 0 to continue, 1 done, negative errno on error.
 *
 * @param[in,out] r Parser.
 * @param[in]     c Byte to feed.
 * @return 0 to continue, 1 when complete, negative errno on error.
 */
static int feed_byte(http_resp_t *r, char c)
{
    switch (r->state) {
    case HR_STATUS_LINE:
    case HR_HEADERS:
    case HR_BODY_CHUNK_SIZE:
    case HR_TRAILERS:
        return feed_line_byte(r, c);

    case HR_BODY_LENGTH:
    case HR_BODY_CHUNK_DATA: {
        int rc = body_append(r, &c, 1);
        if (rc != 0) {
            r->state = HR_ERROR;
            return rc;
        }
        if (--r->remaining == 0) {
            r->state = (r->state == HR_BODY_LENGTH) ? HR_DONE : HR_BODY_CHUNK_CRLF;
        }
        return r->state == HR_DONE ? 1 : 0;
    }

    case HR_BODY_CHUNK_CRLF:
        /* Expect CR then LF; tolerate a bare LF. */
        if (c == '\r') {
            return 0;
        }
        if (c == '\n') {
            r->state = HR_BODY_CHUNK_SIZE;
            return 0;
        }
        r->state = HR_ERROR;
        return -EPROTO;

    case HR_DONE:
        return 1;

    case HR_ERROR:
    default:
        return -EPROTO;
    }
}

int http_resp_feed(http_resp_t *r, const char *buf, size_t len, size_t *consumed)
{
    if (r == NULL || buf == NULL || consumed == NULL) {
        return -EINVAL;
    }
    *consumed = 0;
    if (r->state == HR_DONE) {
        return 1;
    }
    if (r->state == HR_ERROR) {
        return -EPROTO;
    }

    for (size_t i = 0; i < len; ++i) {
        /* Count everything up to the end of headers against the header cap. */
        if (r->state == HR_STATUS_LINE || r->state == HR_HEADERS) {
            if (++r->header_bytes > r->max_header) {
                r->state = HR_ERROR;
                *consumed = i;
                return -EPROTO;
            }
        }
        int rc = feed_byte(r, buf[i]);
        *consumed = i + 1;
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* --- Inbound request parser (webhook mode, SPEC section 4.1) --------------- */

/**
 * @brief Append @p n bytes to an inbound request body, enforcing the body cap.
 *
 * @param[in,out] r   Request parser.
 * @param[in]     buf Bytes to append.
 * @param[in]     n   Count.
 * @return 0 on success, -EPROTO if the cap is exceeded, -ENOMEM on failure.
 */
static int req_body_append(http_req_t *r, const char *buf, size_t n)
{
    if (r->body_len + n > r->max_body) {
        return -EPROTO;
    }
    if (r->body_len + n + 1u > r->body_cap) {
        size_t cap = r->body_cap == 0 ? 256u : r->body_cap;
        while (cap < r->body_len + n + 1u) {
            cap *= 2u;
        }
        char *nb = realloc(r->body, cap);
        if (nb == NULL) {
            return -ENOMEM;
        }
        r->body = nb;
        r->body_cap = cap;
    }
    memcpy(r->body + r->body_len, buf, n);
    r->body_len += n;
    r->body[r->body_len] = '\0';
    return 0;
}

/**
 * @brief Parse the request line into method and target.
 *
 * @param[in,out] r Parser (state advanced to HRQ_HEADERS or HRQ_ERROR).
 */
static void handle_request_line(http_req_t *r)
{
    /* Expect "METHOD SP target SP HTTP/1.x". */
    char *sp1 = strchr(r->line, ' ');
    if (sp1 == NULL) {
        r->state = HRQ_ERROR;
        return;
    }
    *sp1 = '\0';
    const char *method = r->line;
    const char *target = sp1 + 1;

    char *sp2 = strchr(target, ' ');
    if (sp2 == NULL) {
        r->state = HRQ_ERROR;
        return;
    }
    *sp2 = '\0';

    if (strcmp(method, "GET") == 0) {
        r->method = HTTP_METHOD_GET;
    } else if (strcmp(method, "POST") == 0) {
        r->method = HTTP_METHOD_POST;
    } else {
        r->method = HTTP_METHOD_UNKNOWN;
    }
    (void)snprintf(r->target, sizeof(r->target), "%s", target);
    r->state = HRQ_HEADERS;
}

/**
 * @brief Apply one inbound header line (Content-Length, Transfer-Encoding, secret).
 *
 * @param[in,out] r Parser (line holds the header text).
 */
static void handle_req_header_line(http_req_t *r)
{
    char *colon = strchr(r->line, ':');
    if (colon == NULL) {
        r->state = HRQ_ERROR;
        return;
    }
    *colon = '\0';
    const char *name = r->line;
    const char *value = colon + 1;
    while (*value == ' ' || *value == '\t') {
        ++value;
    }
    size_t nlen = strlen(name);

    if (nlen == 14 && ci_equal(name, "content-length", 14)) {
        uint64_t len = 0;
        if (parse_u64(value, strlen(value), &len) != 0) {
            r->state = HRQ_ERROR;
            return;
        }
        r->content_length = len;
        r->has_content_length = 1;
    } else if (nlen == 17 && ci_equal(name, "transfer-encoding", 17) &&
               ci_contains(value, "chunked")) {
        /* The Bot API sends Content-Length; a chunked request body is rejected
         * at the end of the headers (SPEC section 4.1). */
        r->chunked = 1;
    } else if (nlen == 31 && ci_equal(name, "x-telegram-bot-api-secret-token", 31)) {
        (void)snprintf(r->secret, sizeof(r->secret), "%s", value);
    }
}

/**
 * @brief Decide the body framing once the header block ends.
 *
 * @param[in,out] r Parser (state advanced to HRQ_BODY or HRQ_DONE/HRQ_ERROR).
 * @return 0 to continue, 1 when complete, negative errno on error.
 */
static int finish_req_headers(http_req_t *r)
{
    if (r->chunked) {
        r->state = HRQ_ERROR;
        return -EPROTO;
    }
    if (r->has_content_length && r->content_length > 0) {
        if (r->content_length > r->max_body) {
            r->state = HRQ_ERROR;
            return -EPROTO;
        }
        r->remaining = r->content_length;
        r->state = HRQ_BODY;
        return 0;
    }
    r->state = HRQ_DONE;
    return 1;
}

/**
 * @brief Act on a completed inbound line, dispatching by the current state.
 *
 * @param[in,out] r Parser.
 * @return 0 to continue, 1 when complete, negative errno on error.
 */
static int handle_req_line(http_req_t *r)
{
    int rc = 0;
    switch (r->state) {
    case HRQ_REQUEST_LINE:
        handle_request_line(r);
        break;
    case HRQ_HEADERS:
        if (r->line_len == 0) {
            rc = finish_req_headers(r);
        } else {
            handle_req_header_line(r);
        }
        break;
    default:
        break;
    }
    r->line_len = 0;
    if (rc < 0) {
        return rc;
    }
    if (r->state == HRQ_ERROR) {
        return -EPROTO;
    }
    return r->state == HRQ_DONE ? 1 : 0;
}

/**
 * @brief Accumulate a byte into the request line buffer; act when a line arrives.
 *
 * @param[in,out] r Parser.
 * @param[in]     c Byte to feed.
 * @return 0 to continue, 1 when complete, negative errno on error.
 */
static int feed_req_line_byte(http_req_t *r, char c)
{
    if (r->line_len + 1u >= sizeof(r->line)) {
        r->state = HRQ_ERROR;
        return -EPROTO;
    }
    if (c != '\n') {
        r->line[r->line_len++] = c;
        return 0;
    }
    if (r->line_len > 0 && r->line[r->line_len - 1] == '\r') {
        r->line[--r->line_len] = '\0';
    } else {
        r->line[r->line_len] = '\0';
    }
    return handle_req_line(r);
}

/**
 * @brief Process one inbound byte; 0 to continue, 1 done, negative errno on error.
 *
 * @param[in,out] r Parser.
 * @param[in]     c Byte to feed.
 * @return 0 to continue, 1 when complete, negative errno on error.
 */
static int feed_req_byte(http_req_t *r, char c)
{
    switch (r->state) {
    case HRQ_REQUEST_LINE:
    case HRQ_HEADERS:
        return feed_req_line_byte(r, c);
    case HRQ_BODY: {
        int rc = req_body_append(r, &c, 1);
        if (rc != 0) {
            r->state = HRQ_ERROR;
            return rc;
        }
        if (--r->remaining == 0) {
            r->state = HRQ_DONE;
        }
        return r->state == HRQ_DONE ? 1 : 0;
    }
    case HRQ_DONE:
        return 1;
    case HRQ_ERROR:
    default:
        return -EPROTO;
    }
}

int http_req_init(http_req_t *r, size_t max_body)
{
    if (r == NULL) {
        return -EINVAL;
    }
    *r = (http_req_t){
        .state = HRQ_REQUEST_LINE,
        .max_header = HTTP_MAX_HEADER_BYTES,
        .max_body = max_body == 0 ? HTTP_MAX_BODY_BYTES : max_body,
    };
    return 0;
}

void http_req_free(http_req_t *r)
{
    if (r == NULL) {
        return;
    }
    free(r->body);
    r->body = NULL;
    r->body_len = 0;
    r->body_cap = 0;
}

int http_req_feed(http_req_t *r, const char *buf, size_t len, size_t *consumed)
{
    if (r == NULL || buf == NULL || consumed == NULL) {
        return -EINVAL;
    }
    *consumed = 0;
    if (r->state == HRQ_DONE) {
        return 1;
    }
    if (r->state == HRQ_ERROR) {
        return -EPROTO;
    }

    for (size_t i = 0; i < len; ++i) {
        if (r->state == HRQ_REQUEST_LINE || r->state == HRQ_HEADERS) {
            if (++r->header_bytes > r->max_header) {
                r->state = HRQ_ERROR;
                *consumed = i;
                return -EPROTO;
            }
        }
        int rc = feed_req_byte(r, buf[i]);
        *consumed = i + 1;
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/**
 * @brief Map an HTTP status to its reason phrase (unknown statuses -> "Error").
 *
 * @param[in] status HTTP status code.
 * @return A static reason phrase.
 */
static const char *http_reason_phrase(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    default:
        return "Error";
    }
}

int http_response_build(char *out, size_t outcap, int status, const char *body, size_t body_len,
                        size_t *outlen)
{
    if (out == NULL || outlen == NULL || (body == NULL && body_len != 0)) {
        return -EINVAL;
    }
    const char *reason = http_reason_phrase(status);
    int n = snprintf(out, outcap,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, reason, body_len);
    if (n < 0) {
        return -EINVAL;
    }
    if ((size_t)n >= outcap) {
        return -ENOSPC;
    }
    if (body_len != 0) {
        if ((size_t)n + body_len >= outcap) {
            return -ENOSPC;
        }
        memcpy(out + n, body, body_len);
        n += (int)body_len;
    }
    out[n] = '\0';
    *outlen = (size_t)n;
    return 0;
}

int http_request_build(char *out, size_t outcap, const char *method, const char *path,
                       const char *host, const char *user_agent, size_t *outlen)
{
    if (out == NULL || method == NULL || path == NULL || host == NULL || outlen == NULL) {
        return -EINVAL;
    }
    int n = snprintf(out, outcap,
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: %s\r\n"
                     "Accept: application/json\r\n"
                     "Connection: keep-alive\r\n"
                     "\r\n",
                     method, path, host, user_agent != NULL ? user_agent : "tgbot");
    if (n < 0) {
        return -EINVAL;
    }
    if ((size_t)n >= outcap) {
        return -ENOSPC;
    }
    *outlen = (size_t)n;
    return 0;
}

int http_request_build_post(char *out, size_t outcap, const char *path, const char *host,
                            const char *user_agent, const char *body, size_t body_len,
                            size_t *outlen)
{
    if (out == NULL || path == NULL || host == NULL || outlen == NULL ||
        (body == NULL && body_len != 0)) {
        return -EINVAL;
    }
    int n = snprintf(out, outcap,
                     "POST %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: %s\r\n"
                     "Accept: application/json\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: keep-alive\r\n"
                     "\r\n",
                     path, host, user_agent != NULL ? user_agent : "tgbot", body_len);
    if (n < 0) {
        return -EINVAL;
    }
    if ((size_t)n >= outcap) {
        return -ENOSPC;
    }
    if (body_len != 0) {
        if ((size_t)n + body_len >= outcap) {
            return -ENOSPC;
        }
        memcpy(out + n, body, body_len);
        n += (int)body_len;
    }
    out[n] = '\0';
    *outlen = (size_t)n;
    return 0;
}
