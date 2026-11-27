/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/app/seam.h"
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
} seam_head_t;

#define SEAM_PAD 0u
#define SEAM_ALIGN sizeof(seam_head_t)
#define SEAM_ROUND(n) (((n) + SEAM_ALIGN - 1) & ~(SEAM_ALIGN - 1))

typedef struct
{
    _Atomic uint32_t wr, rd;
    uint32_t taken; /* the reader's: how far the last take reaches */
    uint32_t size;
    uint8_t *buf;
} seam_queue_t;

static _Alignas(seam_head_t) uint8_t seam_in_buf[4 * SEAM_MOST];
static _Alignas(seam_head_t) uint8_t seam_out_buf[4 * SEAM_MOST];
static seam_queue_t seam_in = {.size = sizeof seam_in_buf, .buf = seam_in_buf};
static seam_queue_t seam_out = {.size = sizeof seam_out_buf, .buf = seam_out_buf};
static void (*seam_wake)(void);

static bool seam_put(seam_queue_t *q, seam_kind_t kind, const void *body, size_t len)
{
    const uint32_t need = (uint32_t)(sizeof(seam_head_t) + SEAM_ROUND(len));
    const uint32_t wr = atomic_load_explicit(&q->wr, memory_order_relaxed);
    const uint32_t rd = atomic_load_explicit(&q->rd, memory_order_acquire);
    const uint32_t at = wr % q->size;
    const uint32_t tail = q->size - at;
    /* A message that would cross the end goes at the start, after a pad. */
    const uint32_t pad = tail < need ? tail : 0;
    if (len > SEAM_MOST || q->size - (wr - rd) < pad + need)
        return false;
    if (pad)
        *(seam_head_t *)(q->buf + at) = (seam_head_t){SEAM_PAD, tail - (uint32_t)sizeof(seam_head_t)};
    uint8_t *p = q->buf + (wr + pad) % q->size;
    *(seam_head_t *)p = (seam_head_t){(uint32_t)kind, (uint32_t)len};
    if (len)
        memcpy(p + sizeof(seam_head_t), body, len);
    atomic_store_explicit(&q->wr, wr + pad + need, memory_order_release);
    return true;
}

static bool seam_take(seam_queue_t *q, seam_msg_t *m)
{
    uint32_t rd = atomic_load_explicit(&q->rd, memory_order_relaxed) + q->taken;
    atomic_store_explicit(&q->rd, rd, memory_order_release);
    q->taken = 0;
    const uint32_t wr = atomic_load_explicit(&q->wr, memory_order_acquire);
    while (rd != wr)
    {
        const seam_head_t *h = (const seam_head_t *)(q->buf + rd % q->size);
        const uint32_t step = (uint32_t)(sizeof(seam_head_t) + SEAM_ROUND(h->len));
        if (h->kind == SEAM_PAD)
        {
            rd += step;
            atomic_store_explicit(&q->rd, rd, memory_order_release);
            continue;
        }
        m->kind = (seam_kind_t)h->kind;
        m->len = h->len;
        m->body = h + 1;
        q->taken = step;
        return true;
    }
    return false;
}

bool seam_to_machine(seam_kind_t kind, const void *body, size_t len)
{
    const bool put = seam_put(&seam_in, kind, body, len);
    if (put && seam_wake)
        seam_wake();
    return put;
}

bool seam_machine_take(seam_msg_t *m)
{
    return seam_take(&seam_in, m);
}

bool seam_machine_waiting(void)
{
    return atomic_load_explicit(&seam_in.wr, memory_order_acquire) !=
           atomic_load_explicit(&seam_in.rd, memory_order_relaxed) + seam_in.taken;
}

bool seam_to_window(seam_kind_t kind, const void *body, size_t len)
{
    return seam_put(&seam_out, kind, body, len);
}

bool seam_window_take(seam_msg_t *m)
{
    return seam_take(&seam_out, m);
}

bool seam_text_to_machine(seam_kind_t kind, const char *utf8)
{
    return seam_to_machine(kind, utf8, strlen(utf8) + 1);
}

bool seam_text_to_window(seam_kind_t kind, const char *utf8)
{
    return seam_to_window(kind, utf8, strlen(utf8) + 1);
}

void seam_set_wake(void (*wake)(void))
{
    seam_wake = wake;
}

static void seam_empty(seam_queue_t *q)
{
    atomic_store(&q->wr, 0);
    atomic_store(&q->rd, 0);
    q->taken = 0;
}

void seam_reset(void)
{
    seam_empty(&seam_in);
    seam_empty(&seam_out);
}
