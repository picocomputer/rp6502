/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PIX_H_
#define _PIX_H_

#include "hardware/pio.h"
#include <stdint.h>
#include <stdbool.h>

void pix_init();
void pix_task();
void pix_stop();
void pix_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac);
bool pix_set_vga(uint32_t disp);
void pix_api_set_xreg();

#define PIX_PIO pio1
#define PIX_SM 1

// Well known PIX devices. 2-6 are for user expansion.
// RIA device 0 is virtual, not on the physical PIX bus.
#define PIX_XRAM_DEV 0
#define PIX_RIA_DEV 0
#define PIX_VGA_DEV 1
#define PIX_IDLE_DEV 7

// Bit 28 always 1, bits [31:29] for device id, etc.
#define PIX_MESSAGE(dev, ch, byte, word) \
    (0x10000000u | (dev << 29u) | (ch << 24) | ((byte) << 16) | (word))

#define PIX_SEND_XRAM(addr, data) \
    PIX_PIO->txf[PIX_SM] = (PIX_MESSAGE(PIX_XRAM_DEV, 0, (data), (addr)))

static inline bool pix_ready()
{
    // PIX TX FIFO is joined to be 8 deep.
    return pio_sm_get_tx_fifo_level(PIX_PIO, PIX_SM) < 6;
}

static inline void pix_send(uint8_t dev3, uint8_t ch4, uint8_t byte, uint16_t word)
{
    assert(ch4 < 16);
    pio_sm_put(PIX_PIO, PIX_SM, PIX_MESSAGE(dev3, ch4, byte, word));
}

static inline void pix_send_blocking(uint8_t dev3, uint8_t ch4, uint8_t byte, uint16_t word)
{
    while (!pix_ready())
        ;
    pix_send(dev3, ch4, byte, word);
}

#endif /* _PIX_H_ */
