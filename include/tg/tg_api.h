/**
 * @file tg_api.h
 * @brief Minimal Telegram Bot API client for M1 (just enough to call getMe).
 *
 * M1 issues a single request over one connection and parses the JSON reply. The
 * connection pool, rate limiter, and retry policy arrive in M2 (SPEC §12); the
 * @ref tg_api_ops_t seam is in place so those can be added without reworking
 * callers or tests.
 *
 * @ingroup api
 */
#ifndef TG_TG_API_H
#define TG_TG_API_H

#include <stddef.h>
#include <stdint.h>

#include "tg/loop.h"
#include "tg/net_conn.h"
#include "tg/util.h"

/** @brief Parsed bot identity from getMe. */
typedef struct {
    int64_t id;           /**< Bot user id. */
    int is_bot;           /**< Always 1 for a bot. */
    char first_name[128]; /**< Bot first name. */
    char username[64];    /**< Bot @username (without '@'). */
} tg_me_t;

/** @brief Result callback: @p status 0 on success, negative errno otherwise. */
typedef void (*tg_api_cb)(int status, const tg_me_t *me, void *ctx);

/** @brief Injectable API client seam (SPEC-ADDENDUM-TOOLCHAIN A3). */
typedef struct {
    int (*get_me)(void *ctx, tg_api_cb cb, void *user);
    void *ctx;
} tg_api_ops_t;

/** @brief Configuration for an API client. */
typedef struct {
    const char *token;      /**< Bot token (never logged). */
    const char *base_url;   /**< Base URL, e.g. "https://api.telegram.org". */
    const char *user_agent; /**< User-Agent header. */
} tg_api_config_t;

typedef struct tg_api tg_api_t;

/**
 * @brief Create an API client.
 *
 * @param[in] cfg   Configuration (token, base URL, user agent).
 * @param[in] ops   Loop vtable that drives connections.
 * @param[in] net   Socket vtable.
 * @param[in] clock Clock vtable.
 *
 * @return A new client, or NULL on failure.
 * @ownership Caller must call tg_api_destroy().
 * @thread_safety Loop thread only.
 */
tg_api_t *tg_api_create(const tg_api_config_t *cfg, const loop_ops_t *ops, const net_ops_t *net,
                        const clock_ops_t *clock);

/**
 * @brief Begin a getMe request.
 *
 * @param[in] api  Client.
 * @param[in] cb   Completion callback (never NULL).
 * @param[in] ctx  Opaque pointer passed to @p cb.
 *
 * @return 0 if the request started, negative errno on failure.
 * @thread_safety Loop thread only.
 */
int tg_api_get_me(tg_api_t *api, tg_api_cb cb, void *ctx);

/**
 * @brief Destroy the client and any in-flight connection.
 *
 * @param[in] api Client (may be NULL).
 * @thread_safety Loop thread only.
 */
void tg_api_destroy(tg_api_t *api);

/**
 * @brief Parse a getMe JSON body into @p out.
 *
 * Exposed for unit testing without a network. Accepts the standard
 * `{"ok":true,"result":{...}}` shape.
 *
 * @param[in]  json JSON bytes.
 * @param[in]  len  Length.
 * @param[out] out  Parsed identity.
 *
 * @return 0 on success, -EPARSE on malformed JSON or a missing result, -EINVAL
 *         on bad arguments.
 * @thread_safety Safe (no shared state).
 */
int tg_api_parse_me(const char *json, size_t len, tg_me_t *out);

#endif /* TG_TG_API_H */
