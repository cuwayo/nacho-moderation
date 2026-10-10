/* api_methods.h - Telegram Bot API method registry derived from src/api_methods.def.
 *
 * SPEC-ADDENDUM-SSOT S4. The set of API methods the bot uses lives in exactly one
 * place (src/api_methods.def); this header turns it into the METHOD_* enum and the
 * descriptor table, and api_methods.c provides lookup. Method-name strings, the
 * rate-limit bucket class and the idempotency flag (retry policy) all come from
 * the same table, so the client, the rate limiter, the retry policy, the mock
 * server and the docs cannot drift apart (S1, S10).
 *
 * @file api_methods.h
 * @brief API method identifiers, descriptors, and lookup.
 * @ingroup api
 */
#ifndef NM_API_METHODS_H
#define NM_API_METHODS_H

#include <stddef.h>

#include "tables.h"

/** @brief API method identifiers, one per api_methods.def row, plus METHOD_COUNT. */
typedef enum {
#define M(id, method, bucket, idem, brief) METHOD_##id,
#include "api_methods.def"
#undef M
    METHOD_COUNT /**< Number of registered API methods. */
} method_id_t;

/** @brief Static description of one API method. All pointers are to string literals. */
typedef struct {
    method_id_t id;    /**< Stable identifier (METHOD_<id>). */
    const char *name;  /**< Exact Bot API method name used in the request path. */
    bucket_t bucket;   /**< Rate-limit bucket class. */
    int idempotent;    /**< 1 if safe to retry on 5xx / network error. */
    const char *brief; /**< One-line description for docs and logs. */
} api_method_t;

/** @brief The API method table in api_methods.def order (length METHOD_COUNT). */
extern const api_method_t api_method_table[METHOD_COUNT];

/**
 * @brief Look up a method descriptor by its Bot API name.
 *
 * @param[in] name Method name (e.g. "getMe"); may be NULL.
 *
 * @return Pointer to the descriptor, or NULL if no method matches.
 * @ownership Returned pointer has static storage; never freed.
 * @thread_safety Read-only; safe from any thread.
 */
const api_method_t *api_method_lookup(const char *name);

/**
 * @brief Report whether a method is safe to retry on 5xx / network error.
 *
 * @param[in] id Method identifier.
 *
 * @return 1 if idempotent, 0 otherwise (including an out-of-range id).
 * @thread_safety Read-only; safe from any thread.
 */
int api_method_is_idempotent(method_id_t id);

/**
 * @brief Return the Bot API name of a method.
 *
 * @param[in] id Method identifier.
 *
 * @return The method name, or "" for an out-of-range id.
 * @ownership Static storage; never freed.
 * @thread_safety Read-only; safe from any thread.
 */
const char *api_method_name(method_id_t id);

#endif /* NM_API_METHODS_H */
