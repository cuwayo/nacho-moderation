/**
 * @file tg_api.c
 * @brief Minimal Bot API client: getMe over one connection.
 *
 * Flow (SPEC §12, M1): parse the base URL -> resolve the host on the resolver
 * worker (signalled via eventfd) -> connect (TLS if https) -> send
 * `GET /bot<token>/getMe` -> parse the JSON reply. The token is inserted only
 * into the request path and never logged.
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
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <yyjson.h>

#include "tg/errors.h"
#include "tg/http.h"
#include "tg/loop.h"
#include "tg/net_conn.h"
#include "tg/resolver.h"
#include "tg/tls.h"
#include "tg/util.h"

#define TG_API_TIMEOUT_MS 15000u
#define TG_API_MAX_URL 512u

struct tg_api {
    tg_api_config_t cfg;      /**< Copied configuration. */
    const loop_ops_t *ops;    /**< Loop vtable. */
    const net_ops_t *net;     /**< Socket vtable. */
    const clock_ops_t *clock; /**< Clock vtable. */

    char scheme[8]; /**< "http" or "https". */
    char host[256]; /**< Host name. */
    char port[8];   /**< Port string. */

    tg_api_cb cb; /**< User callback. */
    void *cb_ctx; /**< User callback context. */
    int status;   /**< Result status. */
    tg_me_t me;   /**< Parsed identity. */

    int efd;              /**< eventfd for the resolver handoff. */
    op_t efd_op;          /**< OP_EVENTFD read context. */
    resolver_t *resolver; /**< In-flight resolver. */
    conn_t *conn;         /**< In-flight connection. */
};

/* --- URL parsing ----------------------------------------------------------- */

static int parse_base_url(tg_api_t *a, const char *url)
{
    const char *p = strstr(url, "://");
    if (p == NULL) {
        return -EINVAL;
    }
    size_t scheme_len = (size_t)(p - url);
    if (scheme_len == 0 || scheme_len >= sizeof(a->scheme)) {
        return -EINVAL;
    }
    memcpy(a->scheme, url, scheme_len);
    a->scheme[scheme_len] = '\0';
    p += 3;

    const char *slash = strchr(p, '/');
    size_t hostport_len = slash == NULL ? strlen(p) : (size_t)(slash - p);
    if (hostport_len == 0 || hostport_len >= sizeof(a->host)) {
        return -EINVAL;
    }
    char hostport[256];
    memcpy(hostport, p, hostport_len);
    hostport[hostport_len] = '\0';

    const char *colon = strchr(hostport, ':');
    const char *default_port = strcmp(a->scheme, "https") == 0 ? "443" : "80";
    if (colon != NULL) {
        size_t hl = (size_t)(colon - hostport);
        if (hl >= sizeof(a->host)) {
            return -EINVAL;
        }
        memcpy(a->host, hostport, hl);
        a->host[hl] = '\0';
        (void)snprintf(a->port, sizeof(a->port), "%s", colon + 1);
    } else {
        (void)snprintf(a->host, sizeof(a->host), "%s", hostport);
        (void)snprintf(a->port, sizeof(a->port), "%s", default_port);
    }
    return 0;
}

/* --- getMe JSON parsing ---------------------------------------------------- */

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

/* --- Callback plumbing ----------------------------------------------------- */

static void finish(tg_api_t *a, int status)
{
    a->status = status;
    if (a->cb != NULL) {
        a->cb(status, status == 0 ? &a->me : NULL, a->cb_ctx);
    }
}

/* --- Connection completion ------------------------------------------------- */

static void on_conn_done(void *ctx, int status)
{
    tg_api_t *a = ctx;
    if (status != 0) {
        finish(a, status);
        return;
    }
    const http_resp_t *resp = conn_response(a->conn);
    if (resp == NULL || resp->body == NULL) {
        finish(a, -ERR_PARSE);
        return;
    }
    int rc = tg_api_parse_me(resp->body, resp->body_len, &a->me);
    finish(a, rc);
}

/* --- Resolver handoff ------------------------------------------------------ */

static void start_connect(tg_api_t *a)
{
    resolver_join(a->resolver);
    struct addrinfo *res = resolver_take(a->resolver);
    if (res == NULL) {
        finish(a, -EHOSTUNREACH);
        return;
    }

    tls_config_t tls = {.host = a->host, .verify = 1};
    int use_tls = strcmp(a->scheme, "https") == 0;
    a->conn = conn_create(a->ops, a->net, a->clock, use_tls ? &tls : NULL, on_conn_done, a);
    if (a->conn == NULL) {
        freeaddrinfo(res);
        finish(a, -ENOMEM);
        return;
    }
    /* Try each resolved address until one connects. M1 uses the first. */
    int rc = conn_connect(a->conn, res->ai_addr, (socklen_t)res->ai_addrlen, TG_API_TIMEOUT_MS);
    freeaddrinfo(res);
    if (rc != 0) {
        finish(a, rc);
        return;
    }

    char path[TG_API_MAX_URL];
    (void)snprintf(path, sizeof(path), "/bot%s/getMe", a->cfg.token);
    char req[TG_API_MAX_URL + 256];
    size_t req_len = 0;
    if (http_request_build(req, sizeof(req), "GET", path, a->host, a->cfg.user_agent, &req_len) !=
        0) {
        finish(a, -ENOSPC);
        return;
    }
    rc = conn_send_request(a->conn, req, req_len);
    if (rc != 0) {
        finish(a, rc);
    }
}

static void on_eventfd(struct op *op, int res)
{
    tg_api_t *a = op->owner;
    /* Re-arm the eventfd read for the next worker notification. */
    (void)a->ops->prep_read_eventfd(a->ops->ctx, &a->efd_op, a->efd);
    if (res >= 0) {
        start_connect(a);
    }
}

/* --- Public API ------------------------------------------------------------ */

tg_api_t *tg_api_create(const tg_api_config_t *cfg, const loop_ops_t *ops, const net_ops_t *net,
                        const clock_ops_t *clock)
{
    if (cfg == NULL || cfg->token == NULL || cfg->base_url == NULL || ops == NULL || net == NULL) {
        return NULL;
    }
    tg_api_t *a = calloc(1, sizeof(*a));
    if (a == NULL) {
        return NULL;
    }
    a->cfg = *cfg;
    a->ops = ops;
    a->net = net;
    a->clock = clock;
    a->efd = -1;
    if (parse_base_url(a, cfg->base_url) != 0) {
        free(a);
        return NULL;
    }
    a->efd_op = (op_t){.type = OP_EVENTFD, .owner = a, .on_complete = on_eventfd, .fd = -1};
    return a;
}

int tg_api_get_me(tg_api_t *a, tg_api_cb cb, void *ctx)
{
    if (a == NULL || cb == NULL) {
        return -EINVAL;
    }
    a->cb = cb;
    a->cb_ctx = ctx;

    a->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (a->efd < 0) {
        return -errno;
    }
    a->efd_op.fd = a->efd;

    a->resolver = resolver_start(a->host, a->port, a->efd);
    if (a->resolver == NULL) {
        close(a->efd);
        a->efd = -1;
        return -ENOMEM;
    }

    /* Arm the eventfd read so the loop wakes when the worker finishes. */
    int rc = a->ops->prep_read_eventfd(a->ops->ctx, &a->efd_op, a->efd);
    if (rc != 0) {
        return rc;
    }
    return 0;
}

void tg_api_destroy(tg_api_t *a)
{
    if (a == NULL) {
        return;
    }
    resolver_free(a->resolver);
    conn_destroy(a->conn);
    if (a->efd >= 0) {
        close(a->efd);
    }
    free(a);
}
