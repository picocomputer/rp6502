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

void manifold_tx_sst_save(sst_cursor_t *c)
{
    const uint32_t len = RING_LEN(&manifold_tx);
    sst_put_u16(c, (uint16_t)len);
    for (uint32_t i = 0; i < MANIFOLD_TX_SIZE; i++)
    {
        manifold_tx_entry_t e = i < len ? *RING_AT(&manifold_tx, i) : (manifold_tx_entry_t){0};
        sst_put_u8(c, e.ch);
        sst_put_u8(c, e.to);
    }
}

bool manifold_tx_sst_load(sst_cursor_t *c)
{
    const uint16_t len = sst_get_u16(c);
    manifold_tx_entry_t e[MANIFOLD_TX_SIZE];
    for (uint32_t i = 0; i < MANIFOLD_TX_SIZE; i++)
    {
        e[i].ch = sst_get_u8(c);
        e[i].to = sst_get_u8(c);
    }
    if (!sst_ok(c) || len > MANIFOLD_TX_SIZE)
        return false;
    RING_RESET(&manifold_tx);
    RING_IN(&manifold_tx, e, len);
    return true;
}
