/**
 * @file util.c
 * @brief Arena allocator, string builder, duration parser, HTML escaping, logger.
 *
 * @ingroup util
 */
#include "tg/util.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#include "tables.h"

/* --- Clock ----------------------------------------------------------------- */

/**
 * @brief Real monotonic clock: CLOCK_MONOTONIC in milliseconds.
 *
 * @param[in] ctx Unused (NULL).
 * @return Milliseconds since an unspecified epoch.
 */
static uint64_t real_now_ms(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000u) + ((uint64_t)ts.tv_nsec / 1000000u);
}

const clock_ops_t util_clock_ops_real = {.now_ms = real_now_ms, .ctx = NULL};

uint64_t clock_now_ms(const clock_ops_t *c)
{
    if (c == NULL || c->now_ms == NULL) {
        return 0;
    }
    return c->now_ms(c->ctx);
}

/* --- Wall clock ------------------------------------------------------------ */

/**
 * @brief Real wall clock: seconds since the Unix epoch.
 *
 * @param[in] ctx Unused (NULL).
 * @return Unix seconds.
 */
static int64_t real_now_unix(void *ctx)
{
    (void)ctx;
    return (int64_t)time(NULL);
}

const wall_clock_ops_t util_wall_clock_real = {.now_unix = real_now_unix, .ctx = NULL};

int64_t wall_now_unix(const wall_clock_ops_t *c)
{
    if (c == NULL || c->now_unix == NULL) {
        return 0;
    }
    return c->now_unix(c->ctx);
}

/* --- Random source --------------------------------------------------------- */

/** @brief splitmix64 state (seeded by @ref util_random_seed). */
static uint64_t g_random_state;

/**
 * @brief splitmix64: advance the state and return the next 64 bits.
 *
 * A small, fast, well-distributed generator; not cryptographically strong, but
 * the captcha nonce is combined with the chat/user ids and is only a
 * short-lived anti-forgery token, so this is sufficient (and, unlike reading
 * /dev/urandom, never blocks the loop).
 *
 * @param[in] ctx Unused (NULL).
 * @return The next 64 random bits.
 */
static uint64_t real_next_u64(void *ctx)
{
    (void)ctx;
    g_random_state += 0x9E3779B97F4A7C15ULL;
    uint64_t z = g_random_state;
    z = (z ^ (z >> 30u)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27u)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31u);
}

const random_ops_t util_random_ops_real = {.next_u64 = real_next_u64, .ctx = NULL};

uint64_t util_random_u64(const random_ops_t *r)
{
    if (r == NULL || r->next_u64 == NULL) {
        return 0;
    }
    return r->next_u64(r->ctx);
}

void util_random_seed(void)
{
    uint64_t seed = 0;
    ssize_t n = getrandom(&seed, sizeof(seed), 0);
    if (n != (ssize_t)sizeof(seed)) {
        /* Fallback: mix the wall clock and a stack address. Only reached when
         * getrandom() is unavailable; still varied enough for a short-lived
         * nonce. */
        uint64_t addr = (uint64_t)(uintptr_t)&seed;
        seed = (uint64_t)time(NULL) ^ (addr << 17u);
    }
    if (seed == 0) {
        seed = 0x2545F4914F6CDD1DULL; /* never leave the state at zero */
    }
    g_random_state = seed;
}

/* --- Arena ----------------------------------------------------------------- */

/** @brief Arena block alignment (bytes); keeps every allocation suitably aligned. */
#define ARENA_ALIGN 16u
/** @brief Round @p n up to the arena block alignment. */
#define ARENA_ALIGN_UP(n) (((n) + (ARENA_ALIGN - 1u)) & ~(size_t)(ARENA_ALIGN - 1u))

/** @brief One block in the arena's free list. */
struct arena_block {
    arena_block_t *next; /**< Previous block (NULL for the first). */
    size_t used;         /**< Bytes used in this block. */
    size_t cap;          /**< Bytes available in data[]. */
    /* Force the payload to be max_align_t-aligned; without this the 24-byte
     * header would leave data[] only 8-byte aligned, breaking callers that
     * expect suitably aligned allocations (e.g. for int64_t/double). */
    _Alignas(max_align_t) unsigned char data[]; /**< Block storage. */
};

/**
 * @brief Allocate a new arena block with @p cap payload bytes.
 *
 * @param[in] cap Payload capacity in bytes.
 * @return The new block, or NULL on out-of-memory.
 */
static arena_block_t *arena_block_new(size_t cap)
{
    arena_block_t *b = malloc(sizeof(*b) + cap);
    if (b == NULL) {
        return NULL;
    }
    b->next = NULL;
    b->used = 0;
    b->cap = cap;
    return b;
}

int arena_init(arena_t *a, size_t block_size)
{
    if (a == NULL) {
        return -EINVAL;
    }
    if (block_size < 256u) {
        block_size = 256u;
    }
    arena_block_t *b = arena_block_new(block_size);
    if (b == NULL) {
        return -ENOMEM;
    }
    a->head = b;
    a->block_size = block_size;
    return 0;
}

void *arena_alloc(arena_t *a, size_t n)
{
    if (a == NULL || a->head == NULL) {
        return NULL;
    }
    n = ARENA_ALIGN_UP(n == 0 ? 1 : n);

    /* INVARIANT: a->head is the only block with free space; older blocks are full. */
    if (a->head->used + n > a->head->cap) {
        size_t cap = a->block_size > n ? a->block_size : n;
        arena_block_t *b = arena_block_new(cap);
        if (b == NULL) {
            return NULL;
        }
        b->next = a->head;
        a->head = b;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += n;
    return p;
}

void arena_reset(arena_t *a)
{
    if (a == NULL) {
        return;
    }
    /* Keep only the first (oldest) block; free the rest. */
    while (a->head != NULL && a->head->next != NULL) {
        arena_block_t *victim = a->head;
        a->head = a->head->next;
        free(victim);
    }
    if (a->head != NULL) {
        a->head->used = 0;
    }
}

void arena_destroy(arena_t *a)
{
    if (a == NULL) {
        return;
    }
    while (a->head != NULL) {
        arena_block_t *victim = a->head;
        a->head = a->head->next;
        free(victim);
    }
    a->block_size = 0;
}

/* --- String builder -------------------------------------------------------- */

int strbuf_init(strbuf_t *b, size_t initial)
{
    if (b == NULL) {
        return -EINVAL;
    }
    b->buf = NULL;
    b->len = 0;
    b->cap = 0;
    if (initial > 0) {
        b->buf = malloc(initial);
        if (b->buf == NULL) {
            return -ENOMEM;
        }
        b->buf[0] = '\0';
        b->cap = initial;
    }
    return 0;
}

/**
 * @brief Ensure room for @p extra more bytes plus the terminating NUL.
 *
 * @param[in,out] b     Builder.
 * @param[in]     extra Additional bytes needed.
 * @return 0 on success, -ENOMEM on failure.
 */
static int strbuf_reserve(strbuf_t *b, size_t extra)
{
    size_t need = b->len + extra + 1u;
    if (need <= b->cap) {
        return 0;
    }
    size_t cap = b->cap == 0 ? 64u : b->cap;
    while (cap < need) {
        cap *= 2u;
    }
    char *nb = realloc(b->buf, cap);
    if (nb == NULL) {
        return -ENOMEM;
    }
    b->buf = nb;
    b->cap = cap;
    if (b->len == 0) {
        b->buf[0] = '\0';
    }
    return 0;
}

int strbuf_append(strbuf_t *b, const char *s, size_t n)
{
    if (b == NULL || (s == NULL && n != 0)) {
        return -EINVAL;
    }
    int rc = strbuf_reserve(b, n);
    if (rc != 0) {
        return rc;
    }
    if (n > 0) {
        memcpy(b->buf + b->len, s, n);
    }
    b->len += n;
    b->buf[b->len] = '\0';
    return 0;
}

int strbuf_append_str(strbuf_t *b, const char *s)
{
    return strbuf_append(b, s, s == NULL ? 0 : strlen(s));
}

/**
 * @brief Append a printf-formatted string to the builder.
 *
 * @param[in,out] b   Builder.
 * @param[in]     fmt printf format.
 * @param[in]     ... Format arguments.
 * @return 0 on success, -ENOMEM on failure, -EINVAL on bad arguments.
 */
int strbuf_appendf(strbuf_t *b, const char *fmt, ...)
{
    if (b == NULL || fmt == NULL) {
        return -EINVAL;
    }
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        return -EINVAL;
    }
    int rc = strbuf_reserve(b, (size_t)need);
    if (rc == 0) {
        (void)vsnprintf(b->buf + b->len, (size_t)need + 1u, fmt, ap2);
        b->len += (size_t)need;
    }
    va_end(ap2);
    return rc;
}

const char *strbuf_cstr(const strbuf_t *b)
{
    if (b == NULL) {
        return "";
    }
    if (b->buf == NULL) {
        return "";
    }
    return b->buf;
}

void strbuf_free(strbuf_t *b)
{
    if (b == NULL) {
        return;
    }
    free(b->buf);
    b->buf = NULL;
    b->len = 0;
    b->cap = 0;
}

/* --- Duration parser ------------------------------------------------------- */

/**
 * @brief Multiply with overflow detection into @p out.
 *
 * @param[in]  a   First factor.
 * @param[in]  b   Second factor.
 * @param[out] out Receives @p a * @p b on success.
 * @return 0 on success, -ERANGE on overflow.
 */
static int mul_overflow(int64_t a, int64_t b, int64_t *out)
{
    if (a != 0 && b > INT64_MAX / a) {
        return -ERANGE;
    }
    *out = a * b;
    return 0;
}

int util_parse_duration(const char *s, int64_t *out_seconds)
{
    if (s == NULL || out_seconds == NULL || *s == '\0') {
        return -EINVAL;
    }
    /* strtoll() skips leading whitespace and accepts a sign; a duration must
     * start with a digit so " 10m" and "+5"/"-5" are rejected. */
    if (s[0] < '0' || s[0] > '9') {
        return -EINVAL;
    }

    errno = 0;
    char *end = NULL;
    long long value = strtoll(s, &end, 10);
    if (end == s || errno == ERANGE || value < 0) {
        return errno == ERANGE ? -ERANGE : -EINVAL;
    }

    int64_t multiplier = 1;
    if (*end != '\0') {
        /* Exactly one unit suffix, nothing after it. */
        if (end[1] != '\0') {
            return -EINVAL;
        }
        switch (*end) {
        case 's':
        case 'S':
            multiplier = 1;
            break;
        case 'm':
        case 'M':
            multiplier = 60;
            break;
        case 'h':
        case 'H':
            multiplier = 3600;
            break;
        case 'd':
        case 'D':
            multiplier = 86400;
            break;
        case 'w':
        case 'W':
            multiplier = 604800;
            break;
        default:
            return -EINVAL;
        }
    }
    return mul_overflow((int64_t)value, multiplier, out_seconds);
}

/* --- HTML escaping --------------------------------------------------------- */

/* Return the HTML entity for @p c, or NULL if it needs no escaping. */
/**
 * @brief Return the HTML entity for a character, or NULL if it needs no escaping.
 *
 * @param[in] c Character to test.
 * @return The replacement entity, or NULL.
 */
static const char *html_entity(char c)
{
    switch (c) {
    case '&':
        return "&amp;";
    case '<':
        return "&lt;";
    case '>':
        return "&gt;";
    case '"':
        return "&quot;";
    default:
        return NULL;
    }
}

int util_html_escape(const char *in, size_t inlen, char *out, size_t outcap, size_t *outlen)
{
    if (in == NULL || out == NULL || outcap == 0 || outlen == NULL) {
        return -EINVAL;
    }

    size_t o = 0;
    int truncated = 0;
    for (size_t i = 0; i < inlen; ++i) {
        const char *rep = html_entity(in[i]);
        if (rep == NULL) {
            if (o + 1u >= outcap) {
                truncated = 1;
                break;
            }
            out[o++] = in[i];
            continue;
        }
        size_t rlen = strlen(rep);
        if (o + rlen >= outcap) {
            truncated = 1;
            break;
        }
        memcpy(out + o, rep, rlen);
        o += rlen;
    }
    out[o] = '\0';
    *outlen = o;
    return truncated ? -ENOSPC : 0;
}

/* --- Logger ---------------------------------------------------------------- */

/** @brief Current minimum log level (messages below it are suppressed). */
static log_level_t g_log_level = LOG_INFO;

/**
 * @brief Return the lowercase name of a log level.
 *
 * @param[in] level Level.
 * @return A static string ("error", "warn", "info", "debug", or "?").
 */
static const char *level_name(log_level_t level)
{
    switch (level) {
    case LOG_ERROR:
        return "error";
    case LOG_WARN:
        return "warn";
    case LOG_INFO:
        return "info";
    case LOG_DEBUG:
        return "debug";
    default:
        return "?";
    }
}

void util_log_set_level(log_level_t level)
{
    g_log_level = level;
}

/**
 * @brief printf-style logging that takes a va_list (see util.h).
 *
 * @param[in] level  Severity.
 * @param[in] module Module name (short, lowercase).
 * @param[in] fmt    printf format.
 * @param[in] ap     Format argument list.
 */
void util_vlog(log_level_t level, const char *module, const char *fmt, va_list ap)
{
    if (level > g_log_level) {
        return;
    }
    struct timespec ts;
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    char stamp[32];
    struct tm tm;
    (void)gmtime_r(&ts.tv_sec, &tm);
    (void)strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm);

    (void)fprintf(stderr, "%s.%03ld %s %s ", stamp, ts.tv_nsec / 1000000L, level_name(level),
                  module != NULL ? module : "-");
    (void)vfprintf(stderr, fmt, ap);
    (void)fputc('\n', stderr);
}

/**
 * @brief Log one structured line to stderr (see util.h).
 *
 * @param[in] level  Severity.
 * @param[in] module Module name (short, lowercase).
 * @param[in] fmt    printf format.
 * @param[in] ...    Format arguments.
 */
void util_log(log_level_t level, const char *module, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    util_vlog(level, module, fmt, ap);
    va_end(ap);
}
