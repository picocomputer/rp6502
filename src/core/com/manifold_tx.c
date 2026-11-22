/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/com/manifold_tx.h"
#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/sys/ring.h"
#include "machine.h"

static RING(manifold_tx_entry_t, MANIFOLD_TX_SIZE) manifold_tx;

/* The counters may be anything that holds no more than the ring. */
static bool manifold_tx_sst_valid(sst_cursor_t *c)
{
    if (RING_LEN(&manifold_tx) > RING_SIZE(&manifold_tx))
        return sst_refuse(c, "a ring holding more than it can");
    return true;
}

#define MANIFOLD_TX_ENTRY_ROWS(Y, e) Y(U8, e.ch) Y(U8, e.to)
#define MANIFOLD_TX_SST_ROWS(X)                                \
    X(U32, manifold_tx.idx.wr)                                 \
    X(U32, manifold_tx.idx.rd)                                 \
    X(STRUCTS, manifold_tx.buf, MANIFOLD_TX_ENTRY_ROWS)        \
    X(VALIDATE, manifold_tx_sst_valid)
SST_TABLE(manifold_tx, MANIFOLD_TX_SST_ROWS)

/* A failed load can leave the counters holding more than the ring. What
 * waits to go out is dropped. */
void manifold_tx_reset(void)
{
    RING_RESET(&manifold_tx);
}

/* Room for a character, a newline's CR LF included. */
bool manifold_cts(void)
{
    return RING_AVAIL(&manifold_tx) >= 2;
}

void HOST_IN_RAM_FUNC manifold_tx_put(uint8_t to, char c)
{
    while (!RING_AVAIL(&manifold_tx))
        manifold_tx_make_room();
    RING_PUT(&manifold_tx, ((manifold_tx_entry_t){.ch = (uint8_t)c, .to = to}));
}

bool manifold_tx_try_put(uint8_t to, char c)
{
    return RING_PUT(&manifold_tx, ((manifold_tx_entry_t){.ch = (uint8_t)c, .to = to}));
}

void manifold_write_to(uint8_t to, const char *buf, size_t n)
{
    while (n--)
        manifold_tx_put(to, *buf++);
    manifold_tx_wrote();
}

size_t HOST_IN_RAM_FUNC manifold_tx_room(void)
{
    return RING_AVAIL(&manifold_tx);
}

size_t HOST_IN_RAM_FUNC manifold_tx_len(void)
{
    return RING_LEN(&manifold_tx);
}

manifold_tx_entry_t HOST_IN_RAM_FUNC manifold_tx_at(size_t i)
{
    return *RING_AT(&manifold_tx, (uint32_t)i);
}

void HOST_IN_RAM_FUNC manifold_tx_skip(size_t n)
{
    RING_SKIP(&manifold_tx, (uint32_t)n);
}
