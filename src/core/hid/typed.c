/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/paste.h"
#include "core/hid/typed.h"
#include "core/sys/ria.h"
#include "core/sys/ring.h"
#include "core/sys/sst.h"
#include "machine.h"

static RING(char, TYPED_QUEUE_SIZE) typed_queue;

/* The counters may be anything that holds no more than the queue. */
static bool typed_sst_valid(sst_cursor_t *c)
{
    if (RING_LEN(&typed_queue) > TYPED_QUEUE_SIZE)
        return sst_refuse(c, "a queue holding more than it can");
    return true;
}

#define TYPED_QUEUE_ROWS(Y, r) \
    Y(U32, r.idx.wr) Y(U32, r.idx.rd) Y(BYTES, r.buf)
#define TYPED_SST(X)                               \
    X(STRUCT, typed_queue, TYPED_QUEUE_ROWS)       \
    X(VALIDATE, typed_sst_valid)
SST_TABLE(typed, TYPED_SST)

/* A failed load can leave the counters holding more than the queue. What
 * was typed is dropped, as COM's RESET drops it. */
void typed_reset(void)
{
    RING_RESET(&typed_queue);
}

/* A Ctrl-C latches SIGINT before the space is checked, so a break is seen
 * even when the queue is too full to take the byte, and it ends a paste
 * going in (paste.h). */
void HOST_IN_RAM_FUNC typed_push(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == 0x03)
        {
            ria_trigger_sigint();
            paste_cut();
        }
    if (n <= RING_AVAIL(&typed_queue))
        RING_IN(&typed_queue, s, (uint32_t)n);
}

size_t HOST_IN_RAM_FUNC typed_free(void)
{
    return RING_AVAIL(&typed_queue);
}

size_t HOST_IN_RAM_FUNC typed_read(char *buf, size_t length)
{
    return RING_OUT(&typed_queue, buf, (uint32_t)length);
}

int HOST_IN_RAM_FUNC typed_peek(size_t offset)
{
    return offset < RING_LEN(&typed_queue)
               ? (unsigned char)*RING_AT(&typed_queue, offset)
               : -1;
}

void HOST_IN_RAM_FUNC typed_clear(void)
{
    RING_SKIP(&typed_queue, RING_LEN(&typed_queue));
}
