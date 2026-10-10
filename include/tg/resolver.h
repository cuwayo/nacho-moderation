/**
 * @file resolver.h
 * @brief Asynchronous name resolution on a worker thread, signalled via eventfd.
 *
 * `getaddrinfo` is blocking, so it runs on a dedicated worker thread; when it
 * finishes the worker writes to an eventfd that the loop reads via io_uring
 * (SPEC §3, §4.4). The loop then calls resolver_take() to collect the result.
 *
 * @ingroup net
 */
#ifndef TG_RESOLVER_H
#define TG_RESOLVER_H

#include <netdb.h>
#include <stdint.h>

/** @brief Resolution status. */
typedef enum {
    RESOLVER_PENDING = 0, /**< Worker still running. */
    RESOLVER_OK,          /**< A result is available. */
    RESOLVER_FAILED       /**< Resolution failed. */
} resolver_status_t;

/** @brief Opaque resolver object (one worker-thread name resolution). */
typedef struct resolver resolver_t;

/**
 * @brief Start resolving @p host:@p port on a worker thread.
 *
 * @param[in] host Host name.
 * @param[in] port Port as a string.
 * @param[in] efd  eventfd to write to when the worker finishes (may be -1 to
 *                 poll with resolver_status() instead).
 *
 * @return A new resolver, or NULL on failure.
 * @ownership Caller must call resolver_free().
 * @thread_safety Safe to create from the loop thread; the worker runs detached
 *                from the caller's perspective.
 */
resolver_t *resolver_start(const char *host, const char *port, int efd);

/**
 * @brief Report the current status (safe to poll from the loop).
 *
 * @param[in] r Resolver.
 *
 * @return Status.
 * @thread_safety Read of an atomic field; safe from any thread.
 */
resolver_status_t resolver_status(const resolver_t *r);

/**
 * @brief Wait for the worker to finish and return its addrinfo list.
 *
 * Only valid once resolver_status() is RESOLVER_OK.
 *
 * @param[in] r Resolver.
 *
 * @return The addrinfo list, or NULL on failure.
 * @ownership The caller owns the list and must free it with freeaddrinfo().
 * @thread_safety Call from the loop thread only.
 */
struct addrinfo *resolver_take(resolver_t *r);

/**
 * @brief Wait for the worker to finish (blocks until it does).
 *
 * @param[in] r Resolver.
 * @thread_safety Call from the loop thread only (or after resolver_status()).
 */
void resolver_join(resolver_t *r);

/**
 * @brief Free the resolver, joining the worker if necessary.
 *
 * @param[in] r Resolver (may be NULL).
 * @thread_safety Loop thread only.
 */
void resolver_free(resolver_t *r);

#endif /* TG_RESOLVER_H */
