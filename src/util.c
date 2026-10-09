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
#include <time.h>

#include "tables.h"

/* --- Clock ----------------------------------------------------------------- */

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

/* --- Arena ----------------------------------------------------------------- */

#define ARENA_ALIGN 16u
#define ARENA_ALIGN_UP(n) (((n) + (ARENA_ALIGN - 1u)) & ~(size_t)(ARENA_ALIGN - 1u))

struct arena_block {
    arena_block_t *next; /**< Previous block (NULL for the first). */
    size_t used;         /**< Bytes used in this block. */
    size_t cap;          /**< Bytes available in data[]. */
    /* Force the payload to be max_align_t-aligned; without this the 24-byte
     * header would leave data[] only 8-byte aligned, breaking callers that
     * expect suitably aligned allocations (e.g. for int64_t/double). */
    _Alignas(max_align_t) unsigned char data[]; /**< Block storage. */
};

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

/* Ensure room for @p extra more bytes plus the terminating NUL. */
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

/* Multiply with overflow detection into *out. */
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

static log_level_t g_log_level = LOG_INFO;

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

void util_log(log_level_t level, const char *module, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    util_vlog(level, module, fmt, ap);
    va_end(ap);
}
