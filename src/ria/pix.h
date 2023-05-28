/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PIX_H_
#define _PIX_H_

#include "ria.h"
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

// Bit 28 always 1, bits [31:29] for payload type
#define PIX_XREG(channel) ((channel << 29u) | 0x10000000u)
#define PIX_XRAM PIX_XREG(0)
#define PIX_IDLE PIX_XREG(7)

static inline bool pix_ready()
{
    // PIX TX FIFO is joined to be 8 deep.
    return pio_sm_get_tx_fifo_level(PIX_PIO, PIX_SM) < 6;
}

static inline void pix_send(uint8_t dev3, uint8_t ch4, uint8_t byte, uint16_t word)
{
    uint32_t data = PIX_XREG(dev3) | ((ch4 & 0xFu) << 24) | ((byte & 0xFFu) << 16) | word;
    pio_sm_put(PIX_PIO, PIX_SM, data);
}

static inline void pix_send_blocking(uint8_t dev3, uint8_t ch4, uint8_t byte, uint16_t word)
{
    while (!pix_ready())
        ;
    pix_send(dev3, ch4, byte, word);
}

#endif /* _PIX_H_ */
