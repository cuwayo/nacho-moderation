/**
 * @file rate_limit.c
 * @brief Token-bucket rate limiter for outbound Bot API requests (SPEC section 4.2).
 *
 * Tokens are stored in micro-token units (1e6 = one token) so the refill is exact
 * integer arithmetic for every bucket class under a deterministic clock. A request
 * is admitted only when the global bucket and the applicable per-chat bucket both
 * have a token; the per-chat buckets live in a small linear table keyed by chat id
 * (evicting the least-recently-used entry when full).
 *
 * @ingroup api
 */
#include "tg/rate_limit.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include "tables.h"
#include "tg/util.h"

/** @brief Micro-tokens per token (fixed-point scale). */
#define RL_MICRO 1000000ULL

/** @brief Global bucket: 25 requests/second. */
#define RL_GLOBAL_CAP 25ULL
/** @brief Global refill rate in micro-tokens per millisecond (25/s). */
#define RL_GLOBAL_RATE_PER_MS 25000ULL
/** @brief Per-chat message bucket: 15 requests/minute. */
#define RL_MSG_CAP 15ULL
/** @brief Per-chat message refill rate in micro-tokens per millisecond (15/min). */
#define RL_MSG_RATE_PER_MS 250ULL
/** @brief Per-chat bulk bucket: 1 request/second. */
#define RL_BULK_CAP 1ULL
/** @brief Per-chat bulk refill rate in micro-tokens per millisecond (1/s). */
#define RL_BULK_RATE_PER_MS 1000ULL

/** @brief Per-chat bucket table capacity (bounded; LRU-evicted when full). */
#define RL_CHAT_CAPACITY 256u

/** @brief One bucket's token state. */
typedef struct {
    uint64_t micro;        /**< Current micro-tokens. */
    uint64_t last_ms;      /**< Timestamp the tokens were last refilled at. */
    uint64_t paused_until; /**< Absolute ms until which the bucket is paused (429). */
} rl_bucket_t;

/** @brief Per-chat bucket pair plus LRU bookkeeping. */
typedef struct {
    int used;         /**< 1 once the slot holds a chat. */
    int64_t chat_id;  /**< Chat this slot describes. */
    uint64_t lru;     /**< Monotonic use counter for eviction. */
    rl_bucket_t msg;  /**< Per-chat message bucket (15/min). */
    rl_bucket_t bulk; /**< Per-chat bulk bucket (1/s). */
} rl_chat_t;

/** @brief The rate limiter: a global bucket plus per-chat message/bulk buckets. */
struct rate_limiter {
    const clock_ops_t *clock;          /**< Injected clock (never the real one in tests). */
    rl_bucket_t global;                /**< Shared global bucket. */
    rl_chat_t chats[RL_CHAT_CAPACITY]; /**< Per-chat buckets. */
    uint64_t lru_tick;                 /**< Monotonic tick for LRU eviction. */
};

/* --- Refill math ----------------------------------------------------------- */

/**
 * @brief Refill @p b toward @p cap_micro at @p rate_per_ms, advancing its timestamp.
 *
 * Saturates so a large clock jump cannot overflow.
 *
 * @param[in,out] b            Bucket to refill.
 * @param[in]     now_ms       Current time.
 * @param[in]     rate_per_ms  Refill rate in micro-tokens per millisecond.
 * @param[in]     cap_micro    Bucket capacity in micro-tokens.
 */
static void bucket_refill(rl_bucket_t *b, uint64_t now_ms, uint64_t rate_per_ms, uint64_t cap_micro)
{
    if (now_ms > b->last_ms) {
        uint64_t delta = now_ms - b->last_ms;
        uint64_t full_delta = cap_micro / rate_per_ms; /* ms to refill from empty */
        if (delta >= full_delta) {
            b->micro = cap_micro;
        } else {
            b->micro += delta * rate_per_ms;
            if (b->micro > cap_micro) {
                b->micro = cap_micro;
            }
        }
        b->last_ms = now_ms;
    }
}

/**
 * @brief Milliseconds until @p b has at least one token.
 *
 * @param[in] b          Bucket (already refilled to @p now).
 * @param[in] rate_per_ms Refill rate in micro-tokens per millisecond.
 *
 * @return Milliseconds to wait (0 if a token is already available).
 */
static uint64_t bucket_wait_ms(const rl_bucket_t *b, uint64_t rate_per_ms)
{
    if (b->micro >= RL_MICRO) {
        return 0;
    }
    uint64_t missing = RL_MICRO - b->micro;
    return (missing + rate_per_ms - 1ULL) / rate_per_ms; /* ceil */
}

/**
 * @brief Report whether @p b is unpaused and holds at least one token.
 *
 * @param[in] b      Bucket (already refilled to @p now_ms).
 * @param[in] now_ms Current time.
 *
 * @return 1 if the bucket admits a request, else 0.
 */
static int bucket_admits(const rl_bucket_t *b, uint64_t now_ms)
{
    return now_ms >= b->paused_until && b->micro >= RL_MICRO;
}

/* --- Per-chat table -------------------------------------------------------- */

/**
 * @brief Find the per-chat bucket slot for @p chat_id.
 *
 * @param[in] lr      Limiter.
 * @param[in] chat_id Chat to find.
 *
 * @return The slot, or NULL if the chat has no bucket yet.
 */
static rl_chat_t *chat_find(rate_limiter_t *lr, int64_t chat_id)
{
    for (size_t i = 0; i < RL_CHAT_CAPACITY; ++i) {
        if (lr->chats[i].used && lr->chats[i].chat_id == chat_id) {
            return &lr->chats[i];
        }
    }
    return NULL;
}

/**
 * @brief Initialize @p c as a new chat's bucket pair, both buckets full.
 *
 * @param[in]     lr      Limiter (for the LRU tick).
 * @param[in,out] c       Slot to initialize.
 * @param[in]     chat_id Chat the slot describes.
 * @param[in]     now_ms  Current time for the refill timestamps.
 */
static void chat_init(rate_limiter_t *lr, rl_chat_t *c, int64_t chat_id, uint64_t now_ms)
{
    c->used = 1;
    c->chat_id = chat_id;
    c->lru = ++lr->lru_tick;
    /* A new chat starts with both buckets full, so the first message is immediate. */
    c->msg = (rl_bucket_t){.micro = RL_MSG_CAP * RL_MICRO, .last_ms = now_ms};
    c->bulk = (rl_bucket_t){.micro = RL_BULK_CAP * RL_MICRO, .last_ms = now_ms};
}

/**
 * @brief Find or create the per-chat bucket slot for @p chat_id.
 *
 * Evicts the least-recently-used entry when the table is full.
 *
 * @param[in] lr      Limiter.
 * @param[in] chat_id Chat to look up.
 * @param[in] now_ms  Current time for a newly created slot.
 *
 * @return The chat's slot (never NULL).
 */
static rl_chat_t *chat_get_or_create(rate_limiter_t *lr, int64_t chat_id, uint64_t now_ms)
{
    rl_chat_t *c = chat_find(lr, chat_id);
    if (c != NULL) {
        c->lru = ++lr->lru_tick;
        return c;
    }
    for (size_t i = 0; i < RL_CHAT_CAPACITY; ++i) {
        if (!lr->chats[i].used) {
            chat_init(lr, &lr->chats[i], chat_id, now_ms);
            return &lr->chats[i];
        }
    }
    /* Table full: evict the least-recently-used entry. */
    size_t victim = 0;
    for (size_t i = 1; i < RL_CHAT_CAPACITY; ++i) {
        if (lr->chats[i].lru < lr->chats[victim].lru) {
            victim = i;
        }
    }
    chat_init(lr, &lr->chats[victim], chat_id, now_ms);
    return &lr->chats[victim];
}

/**
 * @brief Return the per-chat bucket a request class uses.
 *
 * @param[in] c      Chat slot.
 * @param[in] bucket Request bucket class.
 *
 * @return The bucket, or NULL for `BUCKET_GLOBAL`.
 */
static rl_bucket_t *chat_bucket_for(rl_chat_t *c, bucket_t bucket)
{
    switch (bucket) {
    case BUCKET_PER_CHAT:
        return &c->msg;
    case BUCKET_BULK:
        return &c->bulk;
    case BUCKET_GLOBAL:
    default:
        return NULL;
    }
}

/**
 * @brief Refill rate in micro-tokens per millisecond for a bucket class.
 *
 * @param[in] bucket Request bucket class.
 *
 * @return Micro-tokens per millisecond.
 */
static uint64_t bucket_rate_for(bucket_t bucket)
{
    switch (bucket) {
    case BUCKET_PER_CHAT:
        return RL_MSG_RATE_PER_MS;
    case BUCKET_BULK:
        return RL_BULK_RATE_PER_MS;
    case BUCKET_GLOBAL:
    default:
        return RL_GLOBAL_RATE_PER_MS;
    }
}

/**
 * @brief Bucket capacity in micro-tokens for a bucket class.
 *
 * @param[in] bucket Request bucket class.
 *
 * @return Capacity in micro-tokens.
 */
static uint64_t bucket_cap_for(bucket_t bucket)
{
    switch (bucket) {
    case BUCKET_PER_CHAT:
        return RL_MSG_CAP * RL_MICRO;
    case BUCKET_BULK:
        return RL_BULK_CAP * RL_MICRO;
    case BUCKET_GLOBAL:
    default:
        return RL_GLOBAL_CAP * RL_MICRO;
    }
}

/* --- Public API ------------------------------------------------------------ */

rate_limiter_t *rate_limiter_create(const clock_ops_t *clock)
{
    if (clock == NULL) {
        return NULL;
    }
    rate_limiter_t *lr = calloc(1, sizeof(*lr));
    if (lr == NULL) {
        return NULL;
    }
    lr->clock = clock;
    uint64_t now = clock_now_ms(clock);
    /* Every bucket starts full so startup traffic is not artificially delayed. */
    lr->global = (rl_bucket_t){.micro = RL_GLOBAL_CAP * RL_MICRO, .last_ms = now};
    return lr;
}

void rate_limiter_destroy(rate_limiter_t *lr)
{
    free(lr);
}

int rate_limiter_acquire(rate_limiter_t *lr, bucket_t bucket, int64_t chat_id)
{
    if (lr == NULL) {
        return -EINVAL;
    }
    uint64_t now = clock_now_ms(lr->clock);

    bucket_refill(&lr->global, now, RL_GLOBAL_RATE_PER_MS, RL_GLOBAL_CAP * RL_MICRO);
    int global_ok = bucket_admits(&lr->global, now);

    rl_bucket_t *chat_b = NULL;
    int chat_ok = 1;
    if (bucket != BUCKET_GLOBAL) {
        rl_chat_t *c = chat_get_or_create(lr, chat_id, now);
        chat_b = chat_bucket_for(c, bucket);
        bucket_refill(chat_b, now, bucket_rate_for(bucket), bucket_cap_for(bucket));
        chat_ok = bucket_admits(chat_b, now);
    }

    if (!global_ok || !chat_ok) {
        return -EAGAIN;
    }

    lr->global.micro -= RL_MICRO;
    if (chat_b != NULL) {
        chat_b->micro -= RL_MICRO;
    }
    return 0;
}

void rate_limiter_pause(rate_limiter_t *lr, bucket_t bucket, int64_t chat_id,
                        uint64_t retry_after_ms)
{
    if (lr == NULL || retry_after_ms == 0) {
        return;
    }
    uint64_t now = clock_now_ms(lr->clock);
    uint64_t until = now + retry_after_ms;
    if (bucket == BUCKET_GLOBAL) {
        if (until > lr->global.paused_until) {
            lr->global.paused_until = until;
        }
        return;
    }
    rl_chat_t *c = chat_get_or_create(lr, chat_id, now);
    rl_bucket_t *b = chat_bucket_for(c, bucket);
    if (b != NULL && until > b->paused_until) {
        b->paused_until = until;
    }
}

/**
 * @brief When @p b can next admit a request, given a copy of its state.
 *
 * @param[in] b           Bucket.
 * @param[in] now         Current time.
 * @param[in] rate_per_ms Refill rate in micro-tokens per millisecond.
 * @param[in] cap_micro   Bucket capacity in micro-tokens.
 *
 * @return Absolute millisecond timestamp when the bucket next admits a request.
 */
static uint64_t bucket_ready_at(const rl_bucket_t *b, uint64_t now, uint64_t rate_per_ms,
                                uint64_t cap_micro)
{
    rl_bucket_t copy = *b;
    bucket_refill(&copy, now, rate_per_ms, cap_micro);
    uint64_t at = now + bucket_wait_ms(&copy, rate_per_ms);
    return at < copy.paused_until ? copy.paused_until : at;
}

uint64_t rate_limiter_ready_at_ms(const rate_limiter_t *lr, bucket_t bucket, int64_t chat_id)
{
    if (lr == NULL) {
        return 0;
    }
    uint64_t now = clock_now_ms(lr->clock);
    uint64_t ready = now;

    /* Work on copies so a const query does not mutate refill state. */
    uint64_t g_at =
        bucket_ready_at(&lr->global, now, RL_GLOBAL_RATE_PER_MS, RL_GLOBAL_CAP * RL_MICRO);
    if (g_at > ready) {
        ready = g_at;
    }

    if (bucket == BUCKET_GLOBAL) {
        return ready;
    }

    /* A const query must not create a bucket; a chat that has never been seen has
     * full buckets, so it is ready now. */
    for (size_t i = 0; i < RL_CHAT_CAPACITY; ++i) {
        if (!lr->chats[i].used || lr->chats[i].chat_id != chat_id) {
            continue;
        }
        const rl_bucket_t *src = (bucket == BUCKET_BULK) ? &lr->chats[i].bulk : &lr->chats[i].msg;
        uint64_t at = bucket_ready_at(src, now, bucket_rate_for(bucket), bucket_cap_for(bucket));
        if (at > ready) {
            ready = at;
        }
        break;
    }
    return ready;
}

uint64_t rate_limit_backoff_ms(unsigned retry_index)
{
    if (retry_index >= 5u) {
        return 30000u;
    }
    return 1000u << retry_index; /* 1000, 2000, 4000, 8000, 16000 */
}
