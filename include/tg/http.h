/**
 * @file http.h
 * @brief Minimal HTTP/1.1 client: request builder and incremental response parser.
 *
 * The response parser is a pure state machine fed arbitrary byte chunks; it is
 * safe to call with one byte at a time and never blocks (SPEC-ADDENDUM-TOOLCHAIN
 * A5.1). It handles Content-Length and chunked bodies, keep-alive vs
 * `Connection: close`, and enforces the header/body size caps from SPEC section 9.
 *
 * @ingroup http
 */
#ifndef TG_HTTP_H
#define TG_HTTP_H

#include <stddef.h>
#include <stdint.h>

/** @brief Default maximum header block size (bytes), per SPEC section 9. */
#define HTTP_MAX_HEADER_BYTES (64UL * 1024UL)
/** @brief Default maximum response body size (bytes), per SPEC section 9. */
#define HTTP_MAX_BODY_BYTES (1024UL * 1024UL)

/** @brief Body framing of a response. */
typedef enum {
    HTTP_BODY_NONE = 0, /**< No body (e.g. 204, or a HEAD response). */
    HTTP_BODY_LENGTH,   /**< Content-Length delimited. */
    HTTP_BODY_CHUNKED   /**< Transfer-Encoding: chunked. */
} http_body_mode_t;

/** @brief Parser states. */
typedef enum {
    HR_STATUS_LINE = 0, /**< Reading the status line. */
    HR_HEADERS,         /**< Reading header lines. */
    HR_BODY_LENGTH,     /**< Reading a Content-Length body. */
    HR_BODY_CHUNK_SIZE, /**< Reading a chunk-size line. */
    HR_BODY_CHUNK_DATA, /**< Reading chunk data. */
    HR_BODY_CHUNK_CRLF, /**< Consuming the CRLF after chunk data. */
    HR_TRAILERS,        /**< Reading trailer headers after the last chunk. */
    HR_DONE,            /**< Response complete. */
    HR_ERROR            /**< Malformed or oversized response. */
} http_state_t;

/** @brief Accumulated, decoded response. */
typedef struct {
    int status;            /**< HTTP status code. */
    int keep_alive;        /**< 1 if the connection may be reused. */
    http_body_mode_t mode; /**< Body framing. */
    http_state_t state;    /**< Parser state. */
    size_t header_bytes;   /**< Bytes consumed by the status line + headers. */

    /* Decoded body (heap; owned by the response, freed by http_resp_free). */
    char *body;      /**< Decoded body bytes. */
    size_t body_len; /**< Bytes in body. */
    size_t body_cap; /**< Allocated capacity of body. */

    /* Parsing scratch. */
    uint64_t remaining; /**< Bytes left in the current Content-Length/chunk. */
    size_t max_header;  /**< Header size cap. */
    size_t max_body;    /**< Body size cap. */
    char line[512];     /**< Partial-line accumulator. */
    size_t line_len;    /**< Bytes used in line. */
    int chunked_final;  /**< 1 after the zero-length chunk was seen. */
} http_resp_t;

/**
 * @brief Initialize a response parser.
 *
 * @param[out] r        Response to initialize.
 * @param[in]  max_body Body size cap in bytes (0 for HTTP_MAX_BODY_BYTES).
 *
 * @return 0 on success, negative errno on failure.
 * @ownership Caller must call http_resp_free().
 * @thread_safety Not thread-safe.
 */
int http_resp_init(http_resp_t *r, size_t max_body);

/**
 * @brief Feed @p len bytes of raw response data into the parser.
 *
 * @param[in,out] r         Parser.
 * @param[in]     buf       Raw bytes.
 * @param[in]     len       Count.
 * @param[out]    consumed  Receives the number of bytes consumed.
 *
 * @return 0 while more data is needed, 1 when the response is complete,
 *         negative errno (-EPROTO on malformed/oversized, -ENOMEM) on failure.
 * @thread_safety Not thread-safe.
 */
int http_resp_feed(http_resp_t *r, const char *buf, size_t len, size_t *consumed);

/**
 * @brief Free the response body buffer.
 *
 * @param[in,out] r Parser (safe to call repeatedly).
 * @thread_safety Not thread-safe.
 */
void http_resp_free(http_resp_t *r);

/**
 * @brief Build a minimal HTTP/1.1 request into @p out.
 *
 * Emits `METHOD path HTTP/1.1`, a Host header, a User-Agent, and Connection:
 * keep-alive, followed by any caller headers and a blank line. It never includes
 * a body (M1 only issues GETs).
 *
 * @param[out]    out      Output buffer.
 * @param[in]     outcap   Capacity of @p out.
 * @param[in]     method   HTTP method (e.g. "GET").
 * @param[in]     path     Request path (e.g. "/bot<token>/getMe").
 * @param[in]     host     Host header value.
 * @param[in]     user_agent User-Agent header value.
 * @param[out]    outlen   Receives the number of bytes written (excluding NUL).
 *
 * @return 0 on success, -ENOSPC if @p out is too small, -EINVAL on bad arguments.
 * @thread_safety Safe (no shared state).
 */
int http_request_build(char *out, size_t outcap, const char *method, const char *path,
                       const char *host, const char *user_agent, size_t *outlen);

/**
 * @brief Build an HTTP/1.1 POST request with a JSON body into @p out.
 *
 * Emits `POST path HTTP/1.1`, Host, User-Agent, Accept, `Content-Type:
 * application/json`, `Content-Length`, and `Connection: keep-alive`, followed by
 * the body. Used for every Bot API method call (SPEC section 4.2).
 *
 * @param[out] out       Output buffer.
 * @param[in]  outcap    Capacity of @p out.
 * @param[in]  path      Request path (e.g. "/bot<token>/sendMessage").
 * @param[in]  host      Host header value.
 * @param[in]  user_agent User-Agent header value (NULL for a default).
 * @param[in]  body      JSON request body (may be NULL only when @p body_len is 0).
 * @param[in]  body_len  Length of @p body in bytes.
 * @param[out] outlen    Receives the number of bytes written (excluding NUL).
 *
 * @return 0 on success, -ENOSPC if @p out is too small, -EINVAL on bad arguments.
 * @thread_safety Safe (no shared state).
 */
int http_request_build_post(char *out, size_t outcap, const char *path, const char *host,
                            const char *user_agent, const char *body, size_t body_len,
                            size_t *outlen);

#endif /* TG_HTTP_H */
