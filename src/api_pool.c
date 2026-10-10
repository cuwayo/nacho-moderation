/**
 * @file api_pool.c
 * @brief Pooled Bot API client: keep-alive connections, rate limiting, retries.
 *
 * See api_pool.h for the design rules. Each slot owns a @ref conn_t and, while a
 * request is in flight, the @ref api_request_t it is serving. A completion only
 * records the outcome; the user callback and the next send both happen from
 * api_pool_pump(), which keeps the pool free of re-entrancy and lets a callback
 * enqueue follow-up work safely.
 *
 * Request state machine (per request):
 * @verbatim
 *   REQ_QUEUED --dispatch--> REQ_INFLIGHT --completion--> REQ_DONE (callback)
 *        ^                          |                          |
 *        +------ requeue (429/5xx backoff) -------------------+
 * @endverbatim
 *
 * @ingroup api
 */
#include "tg/api_pool.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "api_methods.h"
#include "tg/api_retry.h"
#include "tg/errors.h"
#include "tg/http.h"
#include "tg/loop.h"
#include "tg/net_conn.h"
#include "tg/rate_limit.h"
#include "tg/tg_api.h"
#include "tg/tls.h"
#include "tg/util.h"

/** @brief Request lifecycle states. */
typedef enum {
    REQ_QUEUED = 0, /**< Waiting for a slot and a token. */
    REQ_INFLIGHT,   /**< Assigned to a slot; awaiting the response. */
    REQ_COMPLETED   /**< Response parsed; awaiting callback/reuse in pump(). */
} req_state_t;

/** @brief One queued or in-flight request. */
typedef struct api_request {
    struct api_request *next; /**< FIFO link. */
    method_id_t method;       /**< API method. */
    pool_class_t cls;         /**< Connection class. */
    req_state_t state;        /**< Lifecycle state. */
    int64_t chat_id;          /**< Target chat for per-chat limiting. */
    uint64_t deadline_ms;     /**< Absolute deadline (0 = none). */
    uint64_t ready_at_ms;     /**< Earliest retry time after a backoff (0 = now). */
    unsigned attempts;        /**< Dispatch attempts made so far. */
    char *body;               /**< Owned JSON body. */
    size_t body_len;          /**< Body length. */
    api_pool_cb cb;           /**< User callback. */
    void *ctx;                /**< User context. */
    api_result_t res;         /**< Result filled by the completion handler. */
} api_request_t;

/** @brief One connection slot. */
typedef struct {
    conn_t *conn;       /**< Connection (NULL until first use). */
    api_request_t *req; /**< Request being served, or NULL when idle. */
    pool_class_t cls;   /**< Class this slot serves. */
} pool_slot_t;

/** @brief The API pool: slots, request queue, timer, and copied configuration. */
struct api_pool {
    api_pool_config_t cfg; /**< Copied configuration (addr points at addr_storage). */
    struct sockaddr_storage addr_storage; /**< Owned copy of the resolved address. */
    pool_slot_t *slots;                   /**< Slot array (length cfg.pool_size). */
    api_request_t *head;                  /**< FIFO head of queued requests. */
    api_request_t *tail;                  /**< FIFO tail. */
    size_t queue_len;                     /**< Number of queued requests. */

    op_t timer_op;   /**< Single dispatch/retry timer. */
    int timer_armed; /**< 1 while a timer SQE is outstanding. */
    char host[256];  /**< Host for the Host header / TLS SNI. */
};

/* --- Request helpers ------------------------------------------------------- */

/** @brief Free a request and its owned body. @param[in] r Request (may be NULL). */
static void request_free(api_request_t *r)
{
    if (r == NULL) {
        return;
    }
    free(r->body);
    free(r);
}

/**
 * @brief Append @p r to the request FIFO.
 *
 * @param[in,out] p Pool whose queue receives the request.
 * @param[in]     r Request to append (must not already be queued).
 */
static void queue_push(api_pool_t *p, api_request_t *r)
{
    r->next = NULL;
    if (p->tail == NULL) {
        p->head = r;
    } else {
        p->tail->next = r;
    }
    p->tail = r;
    p->queue_len++;
}

/* --- Slot management ------------------------------------------------------- */

/**
 * @brief Find a free slot of class @p cls.
 *
 * @param[in] p   Pool.
 * @param[in] cls Connection class to match.
 *
 * @return A free slot, or NULL if every slot of that class is busy.
 */
static pool_slot_t *slot_available(api_pool_t *p, pool_class_t cls)
{
    for (unsigned i = 0; i < p->cfg.pool_size; ++i) {
        if (p->slots[i].cls == cls && p->slots[i].req == NULL) {
            return &p->slots[i];
        }
    }
    return NULL;
}

/**
 * @brief Report whether a free slot of class @p cls exists.
 *
 * @param[in] p   Pool.
 * @param[in] cls Connection class.
 *
 * @return 1 if a slot is free, else 0.
 */
static int slot_has_available(api_pool_t *p, pool_class_t cls)
{
    return slot_available(p, cls) != NULL;
}

/* --- Response classification ----------------------------------------------- */

/**
 * @brief Parse @p conn's response body into @p res and classify the outcome.
 *
 * The HTTP status is classified first: 5xx and 429 are transport-level outcomes
 * that may carry an empty or non-JSON body, so they must not be mistaken for a
 * parse failure. Only a 2xx/3xx body is parsed for the `ok` envelope.
 *
 * @param[in]  conn Connection whose response was completed.
 * @param[out] res  Result filled with the status, body and envelope fields.
 *
 * @return 0 on success, else a negative @ref err_class_t classifying the outcome.
 */
static int classify_response(const conn_t *conn, api_result_t *res)
{
    const http_resp_t *resp = conn_response(conn);
    if (resp == NULL) {
        return -ERR_PROTO;
    }
    res->http_status = resp->status;
    res->body = resp->body;
    res->body_len = resp->body_len;

    if (res->http_status == 429) {
        /* Read parameters.retry_after if the body carries it. */
        if (resp->body != NULL) {
            (void)tg_api_parse_result(res->body, res->body_len, res);
        }
        return -ERR_RATE_LIMIT;
    }
    if (res->http_status >= 500) {
        return -ERR_HTTP; /* retryable for idempotent methods */
    }
    if (res->http_status >= 400) {
        return -ERR_TG_API; /* 4xx other than 429: terminal */
    }

    if (resp->body == NULL) {
        return -ERR_PROTO;
    }
    int rc = tg_api_parse_result(res->body, res->body_len, res);
    if (rc != 0) {
        return -ERR_PARSE;
    }
    if (res->ok) {
        return 0;
    }
    if (res->error_code == 429) {
        return -ERR_RATE_LIMIT;
    }
    return -ERR_TG_API;
}

/* --- Completion handling --------------------------------------------------- */

/**
 * @brief Connection completion callback: record the outcome only.
 *
 * The user callback and slot release happen in api_pool_pump(), which keeps the
 * pool free of re-entrancy. The callback context is the owning slot.
 *
 * @param[in] ctx    Owning pool slot.
 * @param[in] status Connection result (0 on success, negative errno otherwise).
 */
static void on_request_done(void *ctx, int status)
{
    pool_slot_t *s = ctx;
    api_request_t *req = s->req;
    if (req == NULL) {
        return; /* a late completion after the request was abandoned */
    }
    if (status == 0) {
        req->res.status = classify_response(s->conn, &req->res);
    } else {
        req->res.status = status;
        req->res.http_status = 0;
        req->res.body = NULL;
        req->res.body_len = 0;
    }
    req->state = REQ_COMPLETED;
}

/* --- Dispatch -------------------------------------------------------------- */

/**
 * @brief Destroy a slot's connection (used on error / non-keep-alive completion).
 *
 * @param[in,out] s Slot whose connection is closed and cleared.
 */
static void slot_drop_conn(pool_slot_t *s)
{
    conn_destroy(s->conn);
    s->conn = NULL;
}

/**
 * @brief Ensure @p s has a connection ready to send.
 *
 * Reuses an idle keep-alive connection when possible, otherwise creates a fresh
 * one (in CONN_IDLE, for the caller to connect).
 *
 * @param[in]     p Pool (for the address, TLS settings and vtables).
 * @param[in,out] s Slot.
 *
 * @return 0 on success, negative errno (-ENOMEM) on failure.
 */
static int slot_ensure_conn(const api_pool_t *p, pool_slot_t *s)
{
    if (s->conn != NULL) {
        conn_state_t st = conn_state(s->conn);
        if (st == CONN_DONE) {
            /* Idle after a completed response: reuse it when keep-alive allows. */
            if (conn_keep_alive(s->conn) && conn_reuse_begin(s->conn) == 0) {
                return 0; /* CONN_HTTP_WRITE, ready to send */
            }
            slot_drop_conn(s);
        } else if (st != CONN_IDLE) {
            /* Mid-request or terminal: replace with a fresh connection. */
            slot_drop_conn(s);
        }
    }
    if (s->conn == NULL) {
        tls_config_t tls = {.host = p->host, .verify = 1};
        s->conn = conn_create(p->cfg.ops, p->cfg.net, p->cfg.clock, p->cfg.use_tls ? &tls : NULL,
                              on_request_done, s);
        return s->conn == NULL ? -ENOMEM : 0; /* CONN_IDLE; caller connects */
    }
    return 0; /* CONN_IDLE or freshly reused CONN_HTTP_WRITE */
}

/**
 * @brief Send @p req on slot @p s, connecting the socket if needed.
 *
 * @param[in]     p   Pool.
 * @param[in,out] s   Slot that will serve the request.
 * @param[in]     req Request to send.
 *
 * @return 0 if dispatched, negative errno if the request must be failed.
 */
static int slot_dispatch(api_pool_t *p, pool_slot_t *s, api_request_t *req)
{
    int rc = slot_ensure_conn(p, s);
    if (rc != 0) {
        return rc;
    }

    s->req = req;
    req->state = REQ_INFLIGHT;
    req->attempts++;

    if (conn_state(s->conn) == CONN_IDLE) {
        rc = conn_connect(s->conn, (const struct sockaddr *)&p->addr_storage, p->cfg.addr_len,
                          p->cfg.request_timeout_ms);
        if (rc != 0) {
            s->req = NULL;
            slot_drop_conn(s);
            return rc;
        }
    }

    char path[512];
    (void)snprintf(path, sizeof(path), "/bot%s/%s", p->cfg.token, api_method_name(req->method));
    char *wire = malloc(1024 + req->body_len);
    if (wire == NULL) {
        s->req = NULL;
        return -ENOMEM;
    }
    size_t wire_len = 0;
    rc = http_request_build_post(wire, 1024 + req->body_len, path, p->host, p->cfg.user_agent,
                                 req->body, req->body_len, &wire_len);
    if (rc == 0) {
        rc = conn_send_request(s->conn, wire, wire_len);
    }
    free(wire);
    if (rc != 0) {
        s->req = NULL;
        slot_drop_conn(s);
        return rc;
    }
    return 0;
}

/* --- Timer ----------------------------------------------------------------- */

/**
 * @brief Dispatch-timer completion: clear the armed flag so the next pump re-arms.
 *
 * @param[in] op  The timer op (its owner is the pool).
 * @param[in] res Timer result (ignored).
 */
static void on_timer(struct op *op, int res)
{
    (void)res;
    api_pool_t *p = op->owner;
    p->timer_armed = 0;
    /* The next api_pool_pump() re-dispatches; nothing to do here. */
}

/**
 * @brief Arm the single dispatch/retry timer for @p delay_ms, if not already armed.
 *
 * @param[in] p        Pool.
 * @param[in] delay_ms Delay in milliseconds (0 is a no-op).
 */
static void arm_timer(api_pool_t *p, uint64_t delay_ms)
{
    if (p->timer_armed || delay_ms == 0) {
        return;
    }
    int rc = p->cfg.ops->prep_timeout(p->cfg.ops->ctx, &p->timer_op, delay_ms);
    if (rc == 0) {
        p->timer_armed = 1;
    }
}

/* --- Pump ------------------------------------------------------------------ */

/**
 * @brief Release a completed request's slot.
 *
 * Keeps a reusable connection for the next dispatch, or drops it.
 *
 * @param[in,out] s Slot to release.
 */
static void slot_release(pool_slot_t *s)
{
    if (!conn_keep_alive(s->conn)) {
        slot_drop_conn(s);
    }
    s->req = NULL;
}

/**
 * @brief Requeue a request after a retryable outcome.
 *
 * Applies the 429 pause (from retry_after) or the exponential backoff, then
 * pushes the request back onto the FIFO.
 *
 * @param[in]     p      Pool.
 * @param[in,out] req    Request to requeue.
 * @param[in]     action Retry action from the retry policy.
 * @param[in]     now    Current time (ms) for the backoff deadline.
 */
static void requeue_for_retry(api_pool_t *p, api_request_t *req, api_retry_action_t action,
                              uint64_t now)
{
    if (req->res.status == -ERR_RATE_LIMIT && req->res.retry_after != 0) {
        rate_limiter_pause(p->cfg.limiter, api_method_table[req->method].bucket, req->chat_id,
                           req->res.retry_after * 1000u);
    }
    if (action == API_RETRY_BACKOFF) {
        req->ready_at_ms = now + rate_limit_backoff_ms(req->attempts - 1);
    }
    req->state = REQ_QUEUED;
    req->res = (api_result_t){0};
    queue_push(p, req);
}

/**
 * @brief Finish requests whose responses arrived.
 *
 * Fires each callback exactly once, then reuses, requeues or drops the connection.
 *
 * @param[in] p Pool.
 */
static void pump_completed(api_pool_t *p)
{
    for (unsigned i = 0; i < p->cfg.pool_size; ++i) {
        pool_slot_t *s = &p->slots[i];
        if (s->req == NULL || s->req->state != REQ_COMPLETED) {
            continue;
        }
        api_request_t *req = s->req;
        uint64_t now = clock_now_ms(p->cfg.clock);
        api_retry_action_t action = api_retry_decide(req->method, req->res.status,
                                                     req->attempts - 1, now, req->deadline_ms);
        int retrying = action != API_RETRY_NO && req->attempts < API_MAX_ATTEMPTS;

        if (!retrying) {
            req->cb(&req->res, req->ctx); /* exactly once, terminal success or failure */
            slot_release(s);
            request_free(req);
            continue;
        }
        slot_release(s);
        requeue_for_retry(p, req, action, now);
    }
}

/**
 * @brief Remove @p r from the request FIFO.
 *
 * @param[in,out] p    Pool.
 * @param[in]     prev Request before @p r in the queue (NULL if @p r is the head).
 * @param[in]     r    Request to unlink.
 */
static void queue_unlink(api_pool_t *p, api_request_t *prev, api_request_t *r)
{
    if (prev == NULL) {
        p->head = r->next;
    } else {
        prev->next = r->next;
    }
    if (r->next == NULL) {
        p->tail = prev;
    }
    p->queue_len--;
    r->next = NULL;
}

/**
 * @brief Dispatch queued requests to free slots, respecting the rate limiter.
 *
 * @param[in] p Pool.
 */
static void pump_dispatch(api_pool_t *p)
{
    uint64_t now = clock_now_ms(p->cfg.clock);
    api_request_t *prev = NULL;
    api_request_t *r = p->head;

    while (r != NULL) {
        api_request_t *next = r->next;
        pool_slot_t *s = (now >= r->ready_at_ms) ? slot_available(p, r->cls) : NULL;
        int token_ok =
            s != NULL && rate_limiter_acquire(p->cfg.limiter, api_method_table[r->method].bucket,
                                              r->chat_id) == 0;
        if (!token_ok) {
            prev = r;
            r = next;
            continue;
        }

        queue_unlink(p, prev, r);
        int rc = slot_dispatch(p, s, r);
        if (rc != 0) {
            /* Could not send: fail the request now. */
            api_result_t res = {.status = rc, .http_status = 0};
            r->cb(&res, r->ctx);
            request_free(r);
        }
        r = next;
    }
}

/**
 * @brief Arm a single timer for the earliest queued request that could progress.
 *
 * @param[in] p Pool.
 */
static void pump_arm_timer(api_pool_t *p)
{
    if (p->timer_armed || p->head == NULL) {
        return;
    }
    uint64_t now = clock_now_ms(p->cfg.clock);
    uint64_t earliest = 0;
    int have = 0;
    for (const api_request_t *r = p->head; r != NULL; r = r->next) {
        if (!slot_has_available(p, r->cls)) {
            continue; /* No slot for this class; a completion will wake us. */
        }
        uint64_t at = r->ready_at_ms;
        uint64_t rate_at = rate_limiter_ready_at_ms(p->cfg.limiter,
                                                    api_method_table[r->method].bucket, r->chat_id);
        if (rate_at > at) {
            at = rate_at;
        }
        if (!have || at < earliest) {
            earliest = at;
            have = 1;
        }
    }
    if (have && earliest > now) {
        arm_timer(p, earliest - now);
    }
}

/* --- Public API ------------------------------------------------------------ */

api_pool_t *api_pool_create(const api_pool_config_t *cfg)
{
    if (cfg == NULL || cfg->ops == NULL || cfg->net == NULL || cfg->clock == NULL ||
        cfg->limiter == NULL || cfg->addr == NULL || cfg->addr_len == 0 || cfg->token == NULL ||
        cfg->host == NULL || cfg->pool_size == 0) {
        return NULL;
    }
    if ((size_t)cfg->addr_len > sizeof(struct sockaddr_storage)) {
        return NULL;
    }
    api_pool_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->cfg = *cfg;
    /* Copy the resolved address so the pool owns it independently of the caller's
     * addrinfo (which is freed right after creation). */
    memcpy(&p->addr_storage, cfg->addr, cfg->addr_len);
    p->cfg.addr = (const struct sockaddr *)&p->addr_storage;
    (void)snprintf(p->host, sizeof(p->host), "%s", cfg->host);

    p->slots = calloc(cfg->pool_size, sizeof(*p->slots));
    if (p->slots == NULL) {
        free(p);
        return NULL;
    }
    /* Slot 0 is dedicated to polling; the rest serve shared traffic. */
    for (unsigned i = 0; i < cfg->pool_size; ++i) {
        p->slots[i].cls = (i == 0) ? POOL_CLASS_POLLING : POOL_CLASS_SHARED;
    }
    p->timer_op = (op_t){.type = OP_TIMEOUT, .owner = p, .on_complete = on_timer, .fd = -1};
    return p;
}

int api_pool_submit(api_pool_t *pool, method_id_t method, const char *body, size_t body_len,
                    int64_t chat_id, pool_class_t cls, uint64_t deadline_ms, api_pool_cb cb,
                    void *ctx)
{
    if (pool == NULL || cb == NULL || (body == NULL && body_len != 0) || (int)method < 0 ||
        (int)method >= (int)METHOD_COUNT) {
        return -EINVAL;
    }
    if (pool->queue_len >= API_POOL_QUEUE_MAX) {
        return -ENOSPC;
    }
    api_request_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return -ENOMEM;
    }
    if (body_len != 0) {
        r->body = malloc(body_len);
        if (r->body == NULL) {
            free(r);
            return -ENOMEM;
        }
        memcpy(r->body, body, body_len);
    }
    r->method = method;
    r->cls = cls;
    r->state = REQ_QUEUED;
    r->chat_id = chat_id;
    r->deadline_ms = deadline_ms;
    r->body_len = body_len;
    r->cb = cb;
    r->ctx = ctx;
    queue_push(pool, r);
    return 0;
}

void api_pool_pump(api_pool_t *pool)
{
    if (pool == NULL) {
        return;
    }
    pump_completed(pool); /* may enqueue follow-up work via callbacks */
    pump_dispatch(pool);
    pump_arm_timer(pool);
}

size_t api_pool_queue_depth(const api_pool_t *pool)
{
    return pool == NULL ? 0 : pool->queue_len;
}

void api_pool_destroy(api_pool_t *pool)
{
    if (pool == NULL) {
        return;
    }
    for (unsigned i = 0; i < pool->cfg.pool_size; ++i) {
        slot_drop_conn(&pool->slots[i]);
        if (pool->slots[i].req != NULL) {
            request_free(pool->slots[i].req);
        }
    }
    api_request_t *r = pool->head;
    while (r != NULL) {
        api_request_t *next = r->next;
        request_free(r);
        r = next;
    }
    free(pool->slots);
    free(pool);
}
