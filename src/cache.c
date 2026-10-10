/**
 * @file cache.c
 * @brief In-memory hash map with TTL (SPEC-ADDENDUM-TOOLCHAIN A3, ADR 0003).
 *
 * Chained hashing with a fixed bucket array and a doubly-linked list ordered by
 * insertion; the tail is evicted when the map is full. TTL is measured with the
 * injected clock so tests are deterministic.
 *
 * @ingroup cache
 */
#include "tg/cache.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tg/util.h"

/** @brief Default entry cap when the caller passes 0. */
#define CACHE_DEFAULT_MAX 1024u

/** @brief Number of hash buckets (power of two; cheap mask instead of modulo). */
#define CACHE_BUCKETS 256u

/** @brief One stored entry. */
typedef struct cache_entry {
    struct cache_entry *hnext; /**< Next entry in the same bucket. */
    struct cache_entry *older; /**< Less recently inserted entry (towards the tail). */
    struct cache_entry *newer; /**< More recently inserted entry (towards the head). */
    char *key;                 /**< Owned NUL-terminated key. */
    void *val;                 /**< Owned value bytes. */
    size_t len;                /**< Value length in bytes. */
    uint64_t expires_at;       /**< Absolute expiry in ms (0 = never). */
} cache_entry_t;

/** @brief The cache: buckets, an insertion-order list, and the clock. */
struct cache {
    const clock_ops_t *clock; /**< Injected clock (not owned). */
    cache_entry_t **buckets;  /**< Bucket array (length CACHE_BUCKETS). */
    cache_entry_t *head;      /**< Most recently inserted entry. */
    cache_entry_t *tail;      /**< Least recently inserted entry (evict first). */
    size_t count;             /**< Number of entries. */
    size_t max_entries;       /**< Eviction threshold. */
};

/**
 * @brief FNV-1a hash of a NUL-terminated key.
 *
 * @param[in] key Key to hash.
 *
 * @return The 64-bit hash.
 */
static uint64_t hash_key(const char *key)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)key; *p != '\0'; ++p) {
        h ^= (uint64_t)*p;
        h *= 1099511628211ULL;
    }
    return h;
}

/**
 * @brief Free one entry and its owned key/value.
 *
 * @param[in] e Entry to free.
 */
static void entry_free(cache_entry_t *e)
{
    free(e->key);
    free(e->val);
    free(e);
}

/**
 * @brief Unlink @p e from its bucket chain and the insertion-order list.
 *
 * @param[in,out] c Cache.
 * @param[in]     e Entry to unlink.
 */
static void entry_unlink(cache_t *c, cache_entry_t *e)
{
    size_t b = (size_t)(hash_key(e->key) & (CACHE_BUCKETS - 1u));
    cache_entry_t **pp = &c->buckets[b];
    while (*pp != NULL) {
        if (*pp == e) {
            *pp = e->hnext;
            break;
        }
        pp = &(*pp)->hnext;
    }
    if (e->older != NULL) {
        e->older->newer = e->newer;
    } else {
        c->tail = e->newer;
    }
    if (e->newer != NULL) {
        e->newer->older = e->older;
    } else {
        c->head = e->older;
    }
    c->count--;
}

/**
 * @brief Push @p e onto the head of the insertion-order list.
 *
 * @param[in,out] c Cache.
 * @param[in]     e Entry to push.
 */
static void entry_push_front(cache_t *c, cache_entry_t *e)
{
    e->older = c->head;
    e->newer = NULL;
    if (c->head != NULL) {
        c->head->newer = e;
    }
    c->head = e;
    if (c->tail == NULL) {
        c->tail = e;
    }
    c->count++;
}

/**
 * @brief Find the entry for @p key, or NULL.
 *
 * @param[in] c   Cache.
 * @param[in] key Key to look up.
 *
 * @return The entry, or NULL.
 */
static cache_entry_t *entry_find(const cache_t *c, const char *key)
{
    size_t b = (size_t)(hash_key(key) & (CACHE_BUCKETS - 1u));
    for (cache_entry_t *e = c->buckets[b]; e != NULL; e = e->hnext) {
        if (strcmp(e->key, key) == 0) {
            return e;
        }
    }
    return NULL;
}

/**
 * @brief Report whether @p e has expired.
 *
 * @param[in] c Cache (for the clock).
 * @param[in] e Entry.
 *
 * @return 1 if expired, else 0.
 */
static int entry_expired(const cache_t *c, const cache_entry_t *e)
{
    return e->expires_at != 0 && clock_now_ms(c->clock) >= e->expires_at;
}

cache_t *cache_create(const clock_ops_t *clock, size_t max_entries)
{
    if (clock == NULL) {
        return NULL;
    }
    cache_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->buckets = (cache_entry_t **)calloc(CACHE_BUCKETS, sizeof(*c->buckets));
    if (c->buckets == NULL) {
        free(c);
        return NULL;
    }
    c->clock = clock;
    c->max_entries = max_entries != 0 ? max_entries : CACHE_DEFAULT_MAX;
    return c;
}

void cache_destroy(cache_t *c)
{
    if (c == NULL) {
        return;
    }
    cache_entry_t *e = c->tail;
    while (e != NULL) {
        cache_entry_t *next = e->newer;
        entry_free(e);
        e = next;
    }
    free((void *)c->buckets);
    free(c);
}

/**
 * @brief Copy @p len bytes of @p val into a fresh heap block.
 *
 * @param[in] val Source bytes.
 * @param[in] len Byte count (0 yields NULL).
 *
 * @return The copy, or NULL when @p len is 0 or on allocation failure.
 */
static void *copy_bytes(const void *val, size_t len)
{
    if (len == 0) {
        return NULL;
    }
    void *copy = malloc(len);
    if (copy != NULL) {
        (void)memcpy(copy, val, len);
    }
    return copy;
}

/**
 * @brief Fill @p e's TTL from @p ttl_ms and the cache clock.
 *
 * @param[in]     c      Cache (for the clock).
 * @param[in,out] e      Entry whose expiry is set.
 * @param[in]     ttl_ms Time-to-live in ms (0 = never expires).
 */
static void entry_set_ttl(const cache_t *c, cache_entry_t *e, uint64_t ttl_ms)
{
    e->expires_at = ttl_ms != 0 ? clock_now_ms(c->clock) + ttl_ms : 0;
}

/**
 * @brief Replace the value of an existing entry in place.
 *
 * @param[in]     c      Cache (for the clock).
 * @param[in,out] e      Entry to update.
 * @param[in]     val    New value bytes.
 * @param[in]     len    New value length.
 * @param[in]     ttl_ms New TTL in ms (0 = never expires).
 *
 * @return 0 on success, -ENOMEM on allocation failure.
 */
static int entry_replace_value(const cache_t *c, cache_entry_t *e, const void *val, size_t len,
                               uint64_t ttl_ms)
{
    void *copy = copy_bytes(val, len);
    if (len != 0 && copy == NULL) {
        return -ENOMEM;
    }
    free(e->val);
    e->val = copy;
    e->len = len;
    entry_set_ttl(c, e, ttl_ms);
    return 0;
}

/**
 * @brief Allocate a new entry for @p key.
 *
 * @param[in] c      Cache (for the clock).
 * @param[in] key    Key (copied).
 * @param[in] val    Value bytes (copied).
 * @param[in] len    Value length.
 * @param[in] ttl_ms TTL in ms (0 = never expires).
 *
 * @return The new entry, or NULL on allocation failure.
 */
static cache_entry_t *entry_new(const cache_t *c, const char *key, const void *val, size_t len,
                                uint64_t ttl_ms)
{
    cache_entry_t *e = calloc(1, sizeof(*e));
    if (e == NULL) {
        return NULL;
    }
    e->key = malloc(strlen(key) + 1u);
    e->val = copy_bytes(val, len);
    if (e->key == NULL || (len != 0 && e->val == NULL)) {
        free(e->key);
        free(e->val);
        free(e);
        return NULL;
    }
    (void)memcpy(e->key, key, strlen(key) + 1u);
    e->len = len;
    entry_set_ttl(c, e, ttl_ms);
    return e;
}

int cache_put(cache_t *c, const char *key, const void *val, size_t len, uint64_t ttl_ms)
{
    if (c == NULL || key == NULL || (val == NULL && len != 0)) {
        return -EINVAL;
    }

    cache_entry_t *e = entry_find(c, key);
    if (e != NULL) {
        return entry_replace_value(c, e, val, len, ttl_ms);
    }

    e = entry_new(c, key, val, len, ttl_ms);
    if (e == NULL) {
        return -ENOMEM;
    }

    /* Evict the oldest entry when at capacity (the key is new here). */
    if (c->count >= c->max_entries && c->tail != NULL) {
        cache_entry_t *victim = c->tail;
        entry_unlink(c, victim);
        entry_free(victim);
    }

    size_t b = (size_t)(hash_key(key) & (CACHE_BUCKETS - 1u));
    e->hnext = c->buckets[b];
    c->buckets[b] = e;
    entry_push_front(c, e);
    return 0;
}

const void *cache_get(cache_t *c, const char *key, size_t *len)
{
    if (c == NULL || key == NULL) {
        return NULL;
    }
    cache_entry_t *e = entry_find(c, key);
    if (e == NULL) {
        return NULL;
    }
    if (entry_expired(c, e)) {
        entry_unlink(c, e);
        entry_free(e);
        return NULL;
    }
    if (len != NULL) {
        *len = e->len;
    }
    return e->val;
}

int cache_remove(cache_t *c, const char *key)
{
    if (c == NULL || key == NULL) {
        return -EINVAL;
    }
    cache_entry_t *e = entry_find(c, key);
    if (e == NULL) {
        return 1;
    }
    entry_unlink(c, e);
    entry_free(e);
    return 0;
}

void cache_expire(cache_t *c)
{
    if (c == NULL) {
        return;
    }
    cache_entry_t *e = c->tail;
    while (e != NULL) {
        cache_entry_t *next = e->newer;
        if (entry_expired(c, e)) {
            entry_unlink(c, e);
            entry_free(e);
        }
        e = next;
    }
}

size_t cache_count(const cache_t *c)
{
    return c == NULL ? 0 : c->count;
}
