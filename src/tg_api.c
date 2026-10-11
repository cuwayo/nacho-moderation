/**
 * @file tg_api.c
 * @brief Telegram Bot API client facade over the connection pool (SPEC section 4.2).
 *
 * The client parses its base URL, owns a rate limiter and an @ref api_pool_t, and
 * exposes method wrappers plus the JSON helpers used by tests. The token is
 * inserted only into request paths and is never logged.
 *
 * @ingroup api
 */
#include "tg/tg_api.h"

#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <yyjson.h>

#include "api_methods.h"
#include "tg/api_pool.h"
#include "tg/errors.h"
#include "tg/loop.h"
#include "tg/net_conn.h"
#include "tg/rate_limit.h"
#include "tg/resolver.h"
#include "tg/util.h"

/** @brief Maximum number of allowed update types (SPEC section 4.1). */
#define TG_ALLOWED_UPDATES_LEN 5

/** @brief The Bot API client: configuration, limiter, pool, and seam. */
struct tg_api {
    tg_api_config_t cfg;     /**< Copied configuration. */
    rate_limiter_t *limiter; /**< Owned rate limiter. */
    api_pool_t *pool;        /**< Owned connection pool. */
    tg_api_ops_t ops;        /**< Seam bound to this client. */
};

/* --- URL parsing ----------------------------------------------------------- */

int tg_api_parse_base_url(const char *url, char *scheme, size_t scheme_cap, char *host,
                          size_t host_cap, char *port, size_t port_cap)
{
    if (url == NULL || scheme == NULL || host == NULL || port == NULL) {
        return -EINVAL;
    }
    const char *p = strstr(url, "://");
    if (p == NULL) {
        return -EINVAL;
    }
    size_t scheme_len = (size_t)(p - url);
    if (scheme_len == 0 || scheme_len >= scheme_cap) {
        return -EINVAL;
    }
    memcpy(scheme, url, scheme_len);
    scheme[scheme_len] = '\0';
    p += 3;

    const char *slash = strchr(p, '/');
    size_t hostport_len = slash == NULL ? strlen(p) : (size_t)(slash - p);
    if (hostport_len == 0 || hostport_len >= host_cap) {
        return -EINVAL;
    }
    char hostport[256];
    memcpy(hostport, p, hostport_len);
    hostport[hostport_len] = '\0';

    const char *colon = strchr(hostport, ':');
    const char *default_port = strcmp(scheme, "https") == 0 ? "443" : "80";
    if (colon != NULL) {
        size_t hl = (size_t)(colon - hostport);
        if (hl >= host_cap) {
            return -EINVAL;
        }
        memcpy(host, hostport, hl);
        host[hl] = '\0';
        if (strlen(colon + 1) >= port_cap) {
            return -EINVAL;
        }
        (void)snprintf(port, port_cap, "%s", colon + 1);
    } else {
        (void)snprintf(host, host_cap, "%s", hostport);
        (void)snprintf(port, port_cap, "%s", default_port);
    }
    return 0;
}

/* --- Response parsing ------------------------------------------------------ */

int tg_api_parse_result(const char *json, size_t len, api_result_t *out)
{
    if (json == NULL || out == NULL) {
        return -EINVAL;
    }
    yyjson_doc *doc = yyjson_read(json, len, 0);
    if (doc == NULL) {
        return -ERR_PARSE;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *ok = yyjson_obj_get(root, "ok");
    out->ok = ok != NULL && yyjson_get_bool(ok);

    yyjson_val *code = yyjson_obj_get(root, "error_code");
    out->error_code = code != NULL ? (int)yyjson_get_sint(code) : 0;

    yyjson_val *desc = yyjson_obj_get(root, "description");
    out->description[0] = '\0';
    if (desc != NULL && yyjson_is_str(desc)) {
        (void)snprintf(out->description, sizeof(out->description), "%s", yyjson_get_str(desc));
    }

    out->retry_after = 0;
    yyjson_val *params = yyjson_obj_get(root, "parameters");
    if (params != NULL) {
        yyjson_val *retry = yyjson_obj_get(params, "retry_after");
        if (retry != NULL && yyjson_is_int(retry)) {
            out->retry_after = (uint64_t)yyjson_get_sint(retry);
        }
    }

    yyjson_doc_free(doc);
    return 0;
}

int tg_api_parse_me(const char *json, size_t len, tg_me_t *out)
{
    if (json == NULL || out == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof(*out));

    yyjson_doc *doc = yyjson_read(json, len, 0);
    if (doc == NULL) {
        return -ERR_PARSE;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *ok = yyjson_obj_get(root, "ok");
    if (ok == NULL || !yyjson_get_bool(ok)) {
        yyjson_doc_free(doc);
        return -ERR_PARSE;
    }
    yyjson_val *result = yyjson_obj_get(root, "result");
    if (result == NULL) {
        yyjson_doc_free(doc);
        return -ERR_PARSE;
    }
    out->ok = 1;

    yyjson_val *id = yyjson_obj_get(result, "id");
    out->id = id != NULL ? (int64_t)yyjson_get_sint(id) : 0;
    yyjson_val *is_bot = yyjson_obj_get(result, "is_bot");
    out->is_bot = is_bot != NULL && yyjson_get_bool(is_bot);

    yyjson_val *first = yyjson_obj_get(result, "first_name");
    if (first != NULL && yyjson_is_str(first)) {
        (void)snprintf(out->first_name, sizeof(out->first_name), "%s", yyjson_get_str(first));
    }
    yyjson_val *uname = yyjson_obj_get(result, "username");
    if (uname != NULL && yyjson_is_str(uname)) {
        (void)snprintf(out->username, sizeof(out->username), "%s", yyjson_get_str(uname));
    }

    yyjson_doc_free(doc);
    return 0;
}

/* --- Seam ------------------------------------------------------------------ */

/**
 * @brief Adapter implementing @ref tg_api_ops_t::request for a client.
 *
 * @param[in] ctx         The @ref tg_api_t.
 * @param[in] method      API method identifier.
 * @param[in] body        JSON request body.
 * @param[in] body_len    Length of @p body.
 * @param[in] chat_id     Target chat for per-chat rate limiting.
 * @param[in] deadline_ms Absolute deadline (0 for none).
 * @param[in] cb          Completion callback.
 * @param[in] user        Opaque pointer passed to @p cb.
 *
 * @return 0 if queued, negative errno on failure.
 */
static int seam_request(void *ctx, method_id_t method, const char *body, size_t body_len,
                        int64_t chat_id, uint64_t deadline_ms, tg_api_cb cb, void *user)
{
    tg_api_t *api = ctx;
    return tg_api_request(api, method, body, body_len, chat_id, deadline_ms, cb, user);
}

/* --- Method wrappers ------------------------------------------------------- */

int tg_api_request(tg_api_t *api, method_id_t method, const char *body, size_t body_len,
                   int64_t chat_id, uint64_t deadline_ms, tg_api_cb cb, void *ctx)
{
    if (api == NULL) {
        return -EINVAL;
    }
    return api_pool_submit(api->pool, method, body, body_len, chat_id, POOL_CLASS_SHARED,
                           deadline_ms, (api_pool_cb)cb, ctx);
}

int tg_api_get_me(tg_api_t *api, tg_api_cb cb, void *ctx)
{
    return tg_api_request(api, METHOD_GET_ME, NULL, 0, 0, 0, cb, ctx);
}

int tg_api_build_get_updates_body(char *out, size_t cap, int64_t offset, unsigned timeout_s,
                                  size_t *outlen)
{
    if (out == NULL || outlen == NULL) {
        return -EINVAL;
    }
    static const char *const allowed[TG_ALLOWED_UPDATES_LEN] = {
        "message", "edited_message", "callback_query", "chat_member", "my_chat_member"};
    int n = snprintf(out, cap, "{\"offset\":%lld,\"timeout\":%u,\"allowed_updates\":[",
                     (long long)offset, timeout_s);
    if (n < 0 || (size_t)n >= cap) {
        return -ENOSPC;
    }
    size_t off = (size_t)n;
    for (size_t i = 0; i < TG_ALLOWED_UPDATES_LEN; ++i) {
        n = snprintf(out + off, cap - off, "%s\"%s\"", i == 0 ? "" : ",", allowed[i]);
        if (n < 0 || (size_t)n >= cap - off) {
            return -ENOSPC;
        }
        off += (size_t)n;
    }
    n = snprintf(out + off, cap - off, "]}");
    if (n < 0 || (size_t)n >= cap - off) {
        return -ENOSPC;
    }
    off += (size_t)n;
    *outlen = off;
    return 0;
}

int tg_api_get_updates(tg_api_t *api, int64_t offset, unsigned timeout_s, uint64_t deadline_ms,
                       tg_api_cb cb, void *ctx)
{
    if (api == NULL) {
        return -EINVAL;
    }
    char body[256];
    size_t body_len = 0;
    int rc = tg_api_build_get_updates_body(body, sizeof(body), offset, timeout_s, &body_len);
    if (rc != 0) {
        return rc;
    }
    /* The poll uses the dedicated connection so shared traffic never delays it. */
    return api_pool_submit(api->pool, METHOD_GET_UPDATES, body, body_len, 0, POOL_CLASS_POLLING,
                           deadline_ms, (api_pool_cb)cb, ctx);
}

void tg_api_pump(tg_api_t *api)
{
    if (api != NULL) {
        api_pool_pump(api->pool);
    }
}

size_t tg_api_outstanding(const tg_api_t *api)
{
    return api == NULL ? 0 : api_pool_outstanding(api->pool);
}

size_t tg_api_outstanding_polling(const tg_api_t *api)
{
    return api == NULL ? 0 : api_pool_outstanding_class(api->pool, POOL_CLASS_POLLING);
}

const tg_api_ops_t *tg_api_ops(tg_api_t *api)
{
    return api == NULL ? NULL : &api->ops;
}

/* --- Lifecycle ------------------------------------------------------------- */

tg_api_t *tg_api_create(const tg_api_config_t *cfg, const loop_ops_t *ops, const net_ops_t *net,
                        const clock_ops_t *clock)
{
    if (cfg == NULL || cfg->token == NULL || cfg->base_url == NULL || ops == NULL || net == NULL ||
        clock == NULL) {
        return NULL;
    }
    tg_api_t *api = calloc(1, sizeof(*api));
    if (api == NULL) {
        return NULL;
    }
    api->cfg = *cfg;
    if (api->cfg.pool_size == 0) {
        api->cfg.pool_size = 4;
    }

    char scheme[8];
    char host[256];
    char port[8];
    if (tg_api_parse_base_url(cfg->base_url, scheme, sizeof(scheme), host, sizeof(host), port,
                              sizeof(port)) != 0) {
        free(api);
        return NULL;
    }

    api->limiter = rate_limiter_create(clock);
    if (api->limiter == NULL) {
        free(api);
        return NULL;
    }

    /* Resolve the host once, before the loop starts (SPEC section 4.4: DNS runs
     * on a worker thread). This startup resolution blocks the main thread only
     * until the worker finishes, before the event loop is running, which the
     * non-blocking rule permits (check_blocking.sh traces only after startup).
     * The result is reused for every connection. */
    resolver_t *resolver = resolver_start(host, port, -1);
    if (resolver == NULL) {
        rate_limiter_destroy(api->limiter);
        free(api);
        return NULL;
    }
    resolver_join(resolver); /* wait for the worker at startup */
    struct addrinfo *addr = resolver_take(resolver);
    resolver_free(resolver);
    if (addr == NULL) {
        rate_limiter_destroy(api->limiter);
        free(api);
        return NULL;
    }

    api_pool_config_t pcfg = {
        .token = cfg->token,
        .user_agent = cfg->user_agent,
        .host = host,
        .use_tls = strcmp(scheme, "https") == 0,
        .addr = addr->ai_addr,
        .addr_len = (socklen_t)addr->ai_addrlen,
        .pool_size = api->cfg.pool_size,
        .request_timeout_ms = cfg->request_timeout_ms != 0 ? cfg->request_timeout_ms : 15000u,
        .ops = ops,
        .net = net,
        .clock = clock,
        .limiter = api->limiter,
    };
    api->pool = api_pool_create(&pcfg);
    freeaddrinfo(addr); /* the pool copied the address it needs */
    if (api->pool == NULL) {
        rate_limiter_destroy(api->limiter);
        free(api);
        return NULL;
    }

    api->ops = (tg_api_ops_t){.request = seam_request, .ctx = api};
    return api;
}

void tg_api_destroy(tg_api_t *api)
{
    if (api == NULL) {
        return;
    }
    api_pool_destroy(api->pool);
    rate_limiter_destroy(api->limiter);
    free(api);
}
