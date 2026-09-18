/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "com.h"
#include "mmio.h"

#include "core/sys/com.h"

#include <stdint.h>

void com_task(void)
{
    uint32_t v;
    while ((v = UART_POP) & 0x100)
    {
        char c = (char)v;
        com_tx_write(&c, 1);
    }

    /* A 6502 read of the UART that finds no byte sets a request, and a byte
     * is offered only then, because a byte taken with com_getchar is gone
     * from the console input that rln also reads. Writing 0x200 clears the
     * request when nothing is queued, so a byte that arrives later waits for
     * another such read. */
    uint32_t st = RX_OFFER;
    if ((st & 3) == 3)
    {
        com_source_t src = COM_SOURCE_ANY;
        int c = com_getchar(&src);
        if (c >= 0)
        {
            RX_OFFER = (uint32_t)c;
        }
        else
        {
            RX_OFFER = 0x200;
        }
    }
}
