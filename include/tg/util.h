/**
 * @file util.h
 * @brief Utilities: per-update arena, string builder, duration parser, HTML
 *        escaping, and the structured logger.
 *
 * These are the small, dependency-free helpers the rest of the bot builds on.
 * Time is only read for log timestamps; all logic that depends on the clock takes
 * an injected @ref clock_ops_t instead (SPEC-ADDENDUM-TOOLCHAIN A3).
 *
 * @ingroup util
 */
#ifndef TG_UTIL_H
#define TG_UTIL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "tables.h"

/* --- Injectable clock (SPEC-ADDENDUM-TOOLCHAIN A3) ------------------------- */

/**
 * @brief Injectable monotonic clock returning milliseconds.
 *
 * Logic modules must never call clock_gettime()/time() directly; they take a
 * @ref clock_ops_t so tests can supply a deterministic clock. The production
 * implementation lives in util.c (`util_clock_ops_real`).
 */
typedef struct {
    uint64_t (*now_ms)(void *ctx); /**< Returns monotonic milliseconds. */
    void *ctx;                     /**< Opaque context for @ref now_ms. */
} clock_ops_t;

/**
 * @brief Read the clock, or 0 if @p c is NULL.
 *
 * @param[in] c Clock, or NULL.
 *
 * @return Current milliseconds.
 * @thread_safety Depends on the implementation.
 */
uint64_t clock_now_ms(const clock_ops_t *c);

/** @brief Real monotonic clock backed by clock_gettime(CLOCK_MONOTONIC). */
extern const clock_ops_t util_clock_ops_real;

/* --- Arena allocator (per-update scratch memory, SPEC section 9) ----------- */

/** @brief A block in an arena's free list. */
typedef struct arena_block arena_block_t;

/** @brief A bump allocator: allocate freely, free everything at once. */
typedef struct {
    arena_block_t *head; /**< Most recent block (allocation starts here). */
    size_t block_size;   /**< Size of blocks allocated after the first. */
} arena_t;

/**
 * @brief Initialize an arena with an initial block of @p block_size bytes.
 *
 * @param[out] a          Arena to initialize.
 * @param[in]  block_size Bytes to allocate per block (rounded up internally).
 *
 * @return 0 on success, -ENOMEM if the first block cannot be allocated.
 * @ownership Caller must call arena_destroy() when finished.
 * @thread_safety Not thread-safe.
 */
int arena_init(arena_t *a, size_t block_size);

/**
 * @brief Allocate @p n bytes, suitably aligned for any type.
 *
 * @param[in,out] a Arena.
 * @param[in]     n Byte count.
 *
 * @return Pointer to the allocation, or NULL on out-of-memory.
 * @ownership Memory is owned by the arena; freed by arena_reset()/arena_destroy().
 * @thread_safety Not thread-safe.
 */
void *arena_alloc(arena_t *a, size_t n);

/**
 * @brief Free every block except the first, returning the arena to its initial
 *        state for reuse on the next update.
 *
 * @param[in,out] a Arena.
 * @thread_safety Not thread-safe.
 */
void arena_reset(arena_t *a);

/**
 * @brief Free all blocks, including the first.
 *
 * @param[in,out] a Arena (safe to call on a zero-initialized arena).
 * @thread_safety Not thread-safe.
 */
void arena_destroy(arena_t *a);

/* --- Growable string builder ----------------------------------------------- */

/** @brief A NUL-terminated, growable byte buffer. */
typedef struct {
    char *buf;  /**< Heap buffer (always NUL-terminated once non-empty). */
    size_t len; /**< Bytes used, excluding the NUL. */
    size_t cap; /**< Bytes allocated. */
} strbuf_t;

/**
 * @brief Initialize @p b with @p initial capacity (0 for lazy growth).
 *
 * @param[out] b       Builder.
 * @param[in]  initial Initial capacity hint in bytes.
 *
 * @return 0 on success, -ENOMEM on failure.
 * @ownership Caller must call strbuf_free().
 * @thread_safety Not thread-safe.
 */
int strbuf_init(strbuf_t *b, size_t initial);

/**
 * @brief Append @p n bytes from @p s.
 *
 * @param[in,out] b Builder.
 * @param[in]     s Bytes to append.
 * @param[in]     n Count.
 *
 * @return 0 on success, -ENOMEM on failure (buffer left unchanged).
 * @thread_safety Not thread-safe.
 */
int strbuf_append(strbuf_t *b, const char *s, size_t n);

/**
 * @brief Append a NUL-terminated string.
 *
 * @param[in,out] b Builder.
 * @param[in]     s String.
 *
 * @return 0 on success, -ENOMEM on failure.
 * @thread_safety Not thread-safe.
 */
int strbuf_append_str(strbuf_t *b, const char *s);

/**
 * @brief Append a printf-formatted string.
 *
 * @param[in,out] b   Builder.
 * @param[in]     fmt printf format.
 * @param[in]     ... Format arguments.
 *
 * @return 0 on success, -ENOMEM on failure.
 * @thread_safety Not thread-safe.
 */
__attribute__((format(printf, 2, 3))) int strbuf_appendf(strbuf_t *b, const char *fmt, ...);

/**
 * @brief Return the NUL-terminated contents (allocating the NUL if needed).
 *
 * @param[in,out] b Builder.
 *
 * @return Pointer to the string, or "" if the buffer is empty.
 * @ownership Owned by the builder; valid until the next append/free.
 * @thread_safety Not thread-safe.
 */
const char *strbuf_cstr(const strbuf_t *b);

/**
 * @brief Free the builder's storage and reset it to empty.
 *
 * @param[in,out] b Builder.
 * @thread_safety Not thread-safe.
 */
void strbuf_free(strbuf_t *b);

/* --- Duration parser (SPEC section 5.2) ------------------------------------ */

/**
 * @brief Parse a duration literal into seconds.
 *
 * Accepts an optional unit suffix: `s` (seconds), `m` (minutes), `h` (hours),
 * `d` (days), `w` (weeks); units are case-insensitive. A bare integer is seconds.
 * "0" is valid and means zero seconds (the caller treats zero as permanent).
 *
 * @param[in]  s            Duration literal (e.g. "30s", "2h", "7d").
 * @param[out] out_seconds  Parsed seconds.
 *
 * @return 0 on success, -EINVAL on empty/garbage input, -ERANGE on overflow.
 * @thread_safety Safe (no shared state).
 */
int util_parse_duration(const char *s, int64_t *out_seconds);

/* --- HTML escaping (SPEC section 9, 10) ------------------------------------ */

/**
 * @brief HTML-escape @p in into @p out.
 *
 * Escapes `&` -> `&amp;`, `<` -> `&lt;`, `>` -> `&gt;`, `"` -> `&quot;`. All other
 * bytes, including UTF-8 continuation bytes, pass through unchanged.
 *
 * @param[in]  in      Input bytes (need not be NUL-terminated).
 * @param[in]  inlen   Input length.
 * @param[out] out     Output buffer.
 * @param[in]  outcap  Capacity of @p out (including space for the NUL).
 * @param[out] outlen  Receives the number of bytes written, excluding the NUL.
 *
 * @return 0 on success, -ENOSPC if @p out is too small (out is left NUL-terminated
 *         with whatever fit), -EINVAL if any pointer is NULL.
 * @thread_safety Safe (no shared state).
 */
int util_html_escape(const char *in, size_t inlen, char *out, size_t outcap, size_t *outlen);

/* --- Logger (SPEC section 3, A8) ------------------------------------------- */

/**
 * @brief Set the minimum level that will be logged.
 *
 * @param[in] level Minimum level; messages below it are suppressed.
 * @thread_safety Not thread-safe (set once at startup).
 */
void util_log_set_level(log_level_t level);

/**
 * @brief Log one structured line to stderr: `ts level module msg`.
 *
 * The bot token is never passed to the logger; callers must redact secrets.
 *
 * @param[in] level  Severity.
 * @param[in] module Module name (short, lowercase).
 * @param[in] fmt    printf format.
 * @param[in] ...    Format arguments.
 * @thread_safety Not thread-safe (writes to stderr).
 */
__attribute__((format(printf, 3, 4))) void util_log(log_level_t level, const char *module,
                                                    const char *fmt, ...);

/** @brief printf-style logging that takes a va_list (used by util_log). */
__attribute__((format(printf, 3, 0))) void util_vlog(log_level_t level, const char *module,
                                                     const char *fmt, va_list ap);

#endif /* TG_UTIL_H */
