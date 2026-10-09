/**
 * @file fake_ring.c
 * @brief Implementation of the loop_ops_t test double (see fake_ring.h).
 *
 * @ingroup loop
 */
#include "fake_ring.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

#include "tg/loop.h"

static int fr_record(fake_ring_t *fr, op_t *op, int fd, const void *buf, size_t len)
{
    if (fr->pending_len >= FAKE_RING_CAPACITY) {
        return -ENOSPC;
    }
    fake_sqe_t *s = &fr->pending[fr->pending_len++];
    s->type = op->type;
    s->op = op;
    s->fd = fd;
    s->buf = buf;
    s->len = len;
    return 0;
}

static int fr_prep_connect(void *ctx, op_t *op, int fd, const struct sockaddr *sa, socklen_t len)
{
    (void)sa;
    (void)len;
    return fr_record(ctx, op, fd, NULL, 0);
}

static int fr_prep_recv(void *ctx, op_t *op, int fd, void *buf, size_t len)
{
    return fr_record(ctx, op, fd, buf, len);
}

static int fr_prep_send(void *ctx, op_t *op, int fd, const void *buf, size_t len)
{
    return fr_record(ctx, op, fd, buf, len);
}

static int fr_prep_timeout(void *ctx, op_t *op, uint64_t ms)
{
    (void)ms;
    return fr_record(ctx, op, -1, NULL, 0);
}

static int fr_prep_link_timeout(void *ctx, op_t *op, uint64_t ms)
{
    (void)ms;
    return fr_record(ctx, op, -1, NULL, 0);
}

static int fr_prep_read_eventfd(void *ctx, op_t *op, int efd)
{
    return fr_record(ctx, op, efd, NULL, 0);
}

static int fr_prep_close(void *ctx, op_t *op, int fd)
{
    return fr_record(ctx, op, fd, NULL, 0);
}

static int fr_submit(void *ctx)
{
    fake_ring_t *fr = ctx;
    size_t n = fr->pending_len;
    for (size_t i = 0; i < n; ++i) {
        if (fr->inflight_len >= FAKE_RING_CAPACITY) {
            return -ENOSPC;
        }
        fr->inflight[fr->inflight_len++] = fr->pending[i];
    }
    fr->pending_len = 0;
    return (int)n;
}

static int fr_wait_cqe(void *ctx, op_t **op, int *res)
{
    (void)ctx;
    (void)op;
    (void)res;
    /* Tests drive completions explicitly via fake_ring_complete*(). */
    return -EAGAIN;
}

const loop_ops_t *fake_ring_init(fake_ring_t *fr)
{
    memset(fr, 0, sizeof(*fr));
    fr->ops = (loop_ops_t){
        .prep_connect = fr_prep_connect,
        .prep_recv = fr_prep_recv,
        .prep_send = fr_prep_send,
        .prep_timeout = fr_prep_timeout,
        .prep_link_timeout = fr_prep_link_timeout,
        .prep_read_eventfd = fr_prep_read_eventfd,
        .prep_close = fr_prep_close,
        .submit = fr_submit,
        .wait_cqe = fr_wait_cqe,
        .ctx = fr,
    };
    return &fr->ops;
}

static int complete_index(fake_ring_t *fr, size_t idx, int res)
{
    fake_sqe_t s = fr->inflight[idx];
    /* Remove by shifting the tail down. */
    for (size_t i = idx; i + 1 < fr->inflight_len; ++i) {
        fr->inflight[i] = fr->inflight[i + 1];
    }
    fr->inflight_len--;
    if (s.op != NULL && s.op->on_complete != NULL) {
        s.op->on_complete(s.op, res);
    }
    return 0;
}

int fake_ring_complete_next(fake_ring_t *fr, int res)
{
    if (fr->inflight_len == 0) {
        return -ENOENT;
    }
    return complete_index(fr, 0, res);
}

int fake_ring_complete(fake_ring_t *fr, op_t *op, int res)
{
    for (size_t i = 0; i < fr->inflight_len; ++i) {
        if (fr->inflight[i].op == op) {
            return complete_index(fr, i, res);
        }
    }
    return -ENOENT;
}

size_t fake_ring_pending_count(const fake_ring_t *fr)
{
    return fr->pending_len;
}

size_t fake_ring_inflight_count(const fake_ring_t *fr)
{
    return fr->inflight_len;
}

int fake_ring_last_type(const fake_ring_t *fr)
{
    if (fr->pending_len > 0) {
        return (int)fr->pending[fr->pending_len - 1].type;
    }
    if (fr->inflight_len > 0) {
        return (int)fr->inflight[fr->inflight_len - 1].type;
    }
    return -1;
}

int fake_ring_submit_all(fake_ring_t *fr)
{
    return fr->ops.submit(fr);
}

int fake_ring_fill_and_complete(fake_ring_t *fr, op_t *op, const void *data, size_t len)
{
    for (size_t i = 0; i < fr->inflight_len; ++i) {
        if (fr->inflight[i].op != op) {
            continue;
        }
        void *buf = (void *)fr->inflight[i].buf;
        if (buf == NULL || len > fr->inflight[i].len) {
            return -EINVAL;
        }
        memcpy(buf, data, len);
        return complete_index(fr, i, (int)len);
    }
    return -ENOENT;
}

op_t *fake_ring_find(const fake_ring_t *fr, op_type_t type)
{
    for (size_t i = 0; i < fr->inflight_len; ++i) {
        if (fr->inflight[i].type == type) {
            return fr->inflight[i].op;
        }
    }
    return NULL;
}
