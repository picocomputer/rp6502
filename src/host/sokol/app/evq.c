/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/app/evq.h"
#ifdef __EMSCRIPTEN__
#include "host/web/link.h"
#endif
#include <stdatomic.h>
#include <string.h>

/* A queue is a ring of bytes with one writer and one reader, after the
 * kfifo the core's RING is, so its size is a power of two. A message is a header and its body, padded to the
 * header's alignment, and never split across the ring's end: one that would
 * be is preceded by a pad to the end. The writer publishes a message by
 * moving wr past it, and the reader frees one by moving rd past it at its
 * next take, so a body taken stays put until then.
 *
 * A message that would cross the end can leave a pad nearly its own size, so
 * each ring is four times the largest message: one always fits once the ring
 * has drained, and the ring to the machine holds two pastes with a frame's
 * events besides. */
typedef struct
{
    uint32_t kind, len;
} evq_head_t;

#define EVQ_PAD 0u
#define EVQ_ALIGN sizeof(evq_head_t)
#define EVQ_ROUND(n) (((n) + EVQ_ALIGN - 1) & ~(EVQ_ALIGN - 1))

typedef struct
{
    _Atomic uint32_t wr, rd;
    uint32_t taken; /* the reader's: how far the last take reaches */
    uint32_t size;
    uint8_t *buf;
} evq_queue_t;

static _Alignas(evq_head_t) uint8_t evq_in_buf[4 * EVQ_MOST];
static _Alignas(evq_head_t) uint8_t evq_out_buf[4 * EVQ_MOST];
static evq_queue_t evq_in = {.size = sizeof evq_in_buf, .buf = evq_in_buf};
static evq_queue_t evq_out = {.size = sizeof evq_out_buf, .buf = evq_out_buf};
static void (*evq_wake)(void);

static bool evq_put(evq_queue_t *q, evq_kind_t kind, const void *body, size_t len)
{
    const uint32_t need = (uint32_t)(sizeof(evq_head_t) + EVQ_ROUND(len));
    const uint32_t wr = atomic_load_explicit(&q->wr, memory_order_relaxed);
    const uint32_t rd = atomic_load_explicit(&q->rd, memory_order_acquire);
    const uint32_t at = wr % q->size;
    const uint32_t tail = q->size - at;
    /* A message that would cross the end goes at the start, after a pad. */
    const uint32_t pad = tail < need ? tail : 0;
    if (len > EVQ_MOST || q->size - (wr - rd) < pad + need)
        return false;
    if (pad)
        *(evq_head_t *)(q->buf + at) = (evq_head_t){EVQ_PAD, tail - (uint32_t)sizeof(evq_head_t)};
    uint8_t *p = q->buf + (wr + pad) % q->size;
    *(evq_head_t *)p = (evq_head_t){(uint32_t)kind, (uint32_t)len};
    if (len)
        memcpy(p + sizeof(evq_head_t), body, len);
    atomic_store_explicit(&q->wr, wr + pad + need, memory_order_release);
    return true;
}

static bool evq_take(evq_queue_t *q, evq_msg_t *m)
{
    uint32_t rd = atomic_load_explicit(&q->rd, memory_order_relaxed) + q->taken;
    atomic_store_explicit(&q->rd, rd, memory_order_release);
    q->taken = 0;
    const uint32_t wr = atomic_load_explicit(&q->wr, memory_order_acquire);
    while (rd != wr)
    {
        const evq_head_t *h = (const evq_head_t *)(q->buf + rd % q->size);
        const uint32_t step = (uint32_t)(sizeof(evq_head_t) + EVQ_ROUND(h->len));
        if (h->kind == EVQ_PAD)
        {
            rd += step;
            atomic_store_explicit(&q->rd, rd, memory_order_release);
            continue;
        }
        m->kind = (evq_kind_t)h->kind;
        m->len = h->len;
        m->body = h + 1;
        q->taken = step;
        return true;
    }
    return false;
}

bool evq_to_machine(evq_kind_t kind, const void *body, size_t len)
{
#ifdef __EMSCRIPTEN__
    /* The page's window sends to the worker's machine. */
    return link_post(kind, body, len);
#endif
    const bool put = evq_put(&evq_in, kind, body, len);
    if (put && evq_wake)
        evq_wake();
    return put;
}

bool evq_machine_take(evq_msg_t *m)
{
    return evq_take(&evq_in, m);
}

bool evq_machine_waiting(void)
{
    return atomic_load_explicit(&evq_in.wr, memory_order_acquire) !=
           atomic_load_explicit(&evq_in.rd, memory_order_relaxed) + evq_in.taken;
}

bool evq_to_window(evq_kind_t kind, const void *body, size_t len)
{
#ifdef __EMSCRIPTEN__
    /* The worker's machine sends to the page's window. */
    return link_post(kind, body, len);
#endif
    return evq_put(&evq_out, kind, body, len);
}

#ifdef __EMSCRIPTEN__
void evq_deliver(evq_kind_t kind, const void *body, size_t len)
{
    evq_put(link_machine() ? &evq_in : &evq_out, kind, body, len);
}
#endif

bool evq_window_take(evq_msg_t *m)
{
    return evq_take(&evq_out, m);
}

bool evq_text_to_machine(evq_kind_t kind, const char *utf8)
{
    return evq_to_machine(kind, utf8, strlen(utf8) + 1);
}

bool evq_text_to_window(evq_kind_t kind, const char *utf8)
{
    return evq_to_window(kind, utf8, strlen(utf8) + 1);
}

void evq_set_wake(void (*wake)(void))
{
    evq_wake = wake;
}

static void evq_empty(evq_queue_t *q)
{
    atomic_store(&q->wr, 0);
    atomic_store(&q->rd, 0);
    q->taken = 0;
}

void evq_reset(void)
{
    evq_empty(&evq_in);
    evq_empty(&evq_out);
}
