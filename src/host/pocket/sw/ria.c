/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria.h"
#include "mmio.h"
#include "core/com/com.h"
#include "core/sys/ria.h"

#include <stdint.h>

void ria_trigger_sigint(void)
{
    REGS_SIGINT = 0;
}

bool ria_get_sigint(void)
{
    return (REGS_SIGINT & 0x4000u) != 0;
}

void ria_uart_rx_offer(uint8_t ch)
{
    RX_OFFER = ch;
}

bool ria_uart_rx_reclaim(uint8_t *ch)
{
    uint32_t v = RX_RECLAIM;
    if (!(v & 0x100))
        return false;
    *ch = (uint8_t)v;
    return true;
}

int ria_uart_rx_peek(void)
{
    uint32_t v = RX_PEEK;
    return v & 0x100 ? (int)(v & 0xFF) : -1;
}

void ria_task(void)
{
    uint32_t v;
    while ((v = UART_POP) & 0x100)
        com_putchar_raw((char)v);

    /* A 6502 read of the UART that finds no byte sets a request, and a byte
     * is staged only then, so a console byte goes to the fabric only after
     * the 6502 has asked for one. Writing 0x200 clears the request when
     * nothing is queued, so a byte that arrives later waits for another such
     * read. */
    uint32_t st = RX_OFFER;
    if ((st & 3) == 3 && !com_rx_stage())
        RX_OFFER = 0x200;
}
