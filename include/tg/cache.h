/**
 * @file cache.h
 * @brief In-memory hash map with TTL, for hot data such as the admin cache.
 *
 * A fixed-capacity chained hash map keyed by strings. Values are opaque byte
 * blobs copied into the map, so the caller owns nothing afterwards. Time is read
 * only through the injected @ref clock_ops_t, never `time()`/`clock_gettime()`
 * (SPEC-ADDENDUM-TOOLCHAIN A3), which makes TTL behaviour deterministic in tests.
 *
 * When the map is at capacity, inserting a new key evicts the least-recently
 * inserted entry, bounding memory (ADR 0003: hot data cached in memory).
 *
 * @ingroup cache
 */
#ifndef TG_CACHE_H
#define TG_CACHE_H

#include <stddef.h>
#include <stdint.h>

#include "tg/util.h"

/** @brief Opaque TTL cache. */
typedef struct cache cache_t;

/**
 * @brief Create a cache.
 *
 * @param[in] clock       Injectable clock for TTL; must outlive the cache.
 * @param[in] max_entries Maximum number of entries before eviction (0 = default).
 *
 * @return A new cache, or NULL on failure.
 * @ownership Caller must call cache_destroy().
 * @thread_safety Loop thread only.
 */
cache_t *cache_create(const clock_ops_t *clock, size_t max_entries);

/**
 * @brief Destroy the cache and every stored value.
 *
 * @param[in] c Cache (may be NULL).
 * @thread_safety Loop thread only.
 */
void cache_destroy(cache_t *c);

/**
 * @brief Insert or replace @p key with a copy of @p val.
 *
 * @param[in,out] c      Cache.
 * @param[in]     key    NUL-terminated key.
 * @param[in]     val    Value bytes to copy (may be NULL only when @p len is 0).
 * @param[in]     len    Length of @p val in bytes.
 * @param[in]     ttl_ms Time-to-live in milliseconds (0 = never expires).
 *
 * @return 0 on success, -EINVAL on bad arguments, -ENOMEM on allocation failure.
 * @ownership @p val is copied; the caller may free it after return.
 * @thread_safety Loop thread only.
 */
int cache_put(cache_t *c, const char *key, const void *val, size_t len, uint64_t ttl_ms);

/**
 * @brief Look up @p key, returning its value if present and unexpired.
 *
 * An expired entry is removed and reported as absent.
 *
 * @param[in]  c   Cache.
 * @param[in]  key NUL-terminated key.
 * @param[out] len Receives the value length (may be NULL).
 *
 * @return Pointer to the stored value, or NULL if absent/expired.
 * @ownership The pointer is owned by the cache and valid until the next
 *            put/remove/expire/destroy on @p c.
 * @thread_safety Loop thread only.
 */
const void *cache_get(cache_t *c, const char *key, size_t *len);

/**
 * @brief Remove @p key if present.
 *
 * @param[in,out] c   Cache.
 * @param[in]     key Key to remove.
 *
 * @return 0 if removed, 1 if absent, -EINVAL on bad arguments.
 * @thread_safety Loop thread only.
 */
int cache_remove(cache_t *c, const char *key);

/**
 * @brief Remove every expired entry.
 *
 * @param[in,out] c Cache.
 * @thread_safety Loop thread only.
 */
void cache_expire(cache_t *c);

/**
 * @brief Return the number of live (possibly expired) entries.
 *
 * @param[in] c Cache.
 *
 * @return Entry count.
 * @thread_safety Loop thread only.
 */
size_t cache_count(const cache_t *c);

#endif /* TG_CACHE_H */
