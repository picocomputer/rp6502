/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What this machine does once a frame about its console: pass on what the
 * 6502 wrote to its UART, and hand the fabric a byte when the 6502 has asked
 * for one. The console itself -- the rings, the bell, the Ctrl-C -- is
 * core/com/com.c, and which source a byte comes from is core/com/pick.c,
 * reading keymap's queue as this machine's keyboard row directly.
 */

#include "com.h"
#include "mmio.h"

#include "core/sys/com.h"

#include <stdint.h>

void com_task(void)
{
    /* Raw: the program speaks wire bytes, and a UART does not translate. */
    uint32_t v;
    while ((v = UART_POP) & 0x100)
    {
        char c = (char)v;
        com_tx_write(&c, 1);
    }

    /* Only on the ask: offering eagerly would commit bytes the console's
     * own readers still want, and an ask with nothing queued is answered
     * with nothing rather than remembered. */
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
