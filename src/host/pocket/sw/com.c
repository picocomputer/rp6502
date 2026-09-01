/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What this machine does once a frame about its console: hand the fabric a
 * byte when the 6502 has asked for one, and take in what the platform's
 * keyboard and the layout engine produced. The console itself -- the rings,
 * the bell, the Ctrl-C -- is core/com/com.c.
 */

#include "com.h"
#include "mmio.h"

#include "core/sys/com.h"
#include "core/hid/keymap.h"

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
     * with nothing rather than remembered. Served before the keyboard
     * poll so a byte cannot arrive inside the same tick as an expired ask. */
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

    /* Bit 8 is the valid flag. Testbench only; nothing on hardware
     * drives this register. */
    uint32_t k = MMIO_KBD;
    if (k & 0x100)
        com_keyboard_push_byte((uint8_t)k);

    char buf[16];
    size_t n = keymap_in_chars(buf, sizeof buf);
    com_keyboard_push(buf, n);
}
