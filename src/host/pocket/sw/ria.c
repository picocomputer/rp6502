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

static bool ria_sigint;

void ria_trigger_sigint(void)
{
    ria_sigint = true;
}

bool ria_get_sigint(void)
{
    bool latched = ria_sigint;
    ria_sigint = false;
    return latched;
}

void ria_uart_rx_offer(uint8_t ch)
{
    RX_OFFER = ch;
}

bool ria_uart_rx_reclaim(uint8_t *ch)
{
    (void)ch;
    return false;
}

int ria_uart_rx_peek(void)
{
    return -1;
}

void ria_task(void)
{
    uint32_t v;
    while ((v = UART_POP) & 0x100)
        com_putchar_raw((char)v);

    /* A 6502 read of the UART that finds no byte sets a request, and a byte
     * is staged only then, because a staged byte is gone from the console
     * input that rln also reads. Writing 0x200 clears the request when
     * nothing is queued, so a byte that arrives later waits for another such
     * read. */
    uint32_t st = RX_OFFER;
    if ((st & 3) == 3 && !com_rx_stage())
        RX_OFFER = 0x200;
}
