/**
 * @file resolver.c
 * @brief Worker-thread name resolution with an eventfd handoff.
 *
 * The worker thread performs the blocking getaddrinfo() call; the loop thread
 * never blocks (SPEC §3, §4.4). A small mutex protects the result handoff; the
 * status is an atomic so the loop can poll it without locking.
 *
 * @ingroup net
 */
#include "tg/resolver.h"

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/** @brief One worker-thread name resolution and its eventfd handoff. */
struct resolver {
    pthread_t thread;        /**< Worker thread. */
    atomic_int status;       /**< RESOLVER_* status. */
    struct addrinfo *result; /**< Worker result (owned; handed off on take). */
    int gai_rc;              /**< getaddrinfo return code. */
    int efd;                 /**< eventfd to signal (-1 for none). */
    int joined;              /**< 1 once the thread has been joined. */
    char host[256];          /**< Copy of the host name. */
    char port[32];           /**< Copy of the port string. */
};

/**
 * @brief Wake the loop by writing to the eventfd (best-effort).
 *
 * @param[in] efd eventfd, or -1 for none.
 */
static void notify(int efd)
{
    if (efd < 0) {
        return;
    }
    uint64_t one = 1;
    ssize_t n = write(efd, &one, sizeof(one));
    (void)n; /* best-effort; a lost wakeup is harmless because status is polled */
}

/**
 * @brief Worker entry point: resolve the host and publish the result.
 *
 * @param[in] arg The resolver.
 * @return NULL (unused).
 */
static void *worker(void *arg)
{
    resolver_t *r = arg;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(r->host, r->port, &hints, &res);
    r->gai_rc = rc;
    r->result = res;
    atomic_store_explicit(&r->status, rc == 0 ? RESOLVER_OK : RESOLVER_FAILED,
                          memory_order_release);
    notify(r->efd);
    return NULL;
}

resolver_t *resolver_start(const char *host, const char *port, int efd)
{
    if (host == NULL || port == NULL) {
        return NULL;
    }
    resolver_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->efd = efd;
    atomic_init(&r->status, RESOLVER_PENDING);
    (void)snprintf(r->host, sizeof(r->host), "%s", host);
    (void)snprintf(r->port, sizeof(r->port), "%s", port);

    if (pthread_create(&r->thread, NULL, worker, r) != 0) {
        free(r);
        return NULL;
    }
    return r;
}

resolver_status_t resolver_status(const resolver_t *r)
{
    if (r == NULL) {
        return RESOLVER_FAILED;
    }
    return (resolver_status_t)atomic_load_explicit(&r->status, memory_order_acquire);
}

struct addrinfo *resolver_take(resolver_t *r)
{
    if (r == NULL || resolver_status(r) != RESOLVER_OK) {
        return NULL;
    }
    struct addrinfo *res = r->result;
    r->result = NULL;
    return res;
}

void resolver_join(resolver_t *r)
{
    if (r == NULL || r->joined) {
        return;
    }
    (void)pthread_join(r->thread, NULL);
    r->joined = 1;
}

void resolver_free(resolver_t *r)
{
    if (r == NULL) {
        return;
    }
    resolver_join(r); /* idempotent: joins at most once */
    if (r->result != NULL) {
        freeaddrinfo(r->result);
    }
    free(r);
}
