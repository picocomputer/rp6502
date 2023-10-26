/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PIX_H_
#define _PIX_H_

#include "main.h"
#include "hardware/pio.h"
#include <stdint.h>
#include <stdbool.h>

/* Kernel events
 */

void pix_init(void);
void pix_stop(void);
void pix_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac);

/* API to set XREGs
 */

void pix_api_xreg(void);
void pix_ack(void);
void pix_nak(void);

// Well known PIX devices. 2-6 are for user expansion.
// RIA device 0 is virtual, not on the physical PIX bus.

#define PIX_DEVICE_XRAM 0
#define PIX_DEVICE_RIA 0
#define PIX_DEVICE_VGA 1
#define PIX_DEVICE_IDLE 7

// Bit 28 always 1, bits [31:29] for device id, etc.
#define PIX_MESSAGE(dev, ch, byte, word) \
    (0x10000000u | (dev << 29u) | (ch << 24) | ((byte) << 16) | (word))

// Macro for the RIA. Use the inline functions elsewhere.
#define PIX_SEND_XRAM(addr, data) \
    PIX_PIO->txf[PIX_SM] = (PIX_MESSAGE(PIX_DEVICE_XRAM, 0, (data), (addr)))

// Test for free space in the PIX transmit FIFO.
static inline bool pix_ready(void)
{
    // PIX TX FIFO is joined to be 8 deep.
    return pio_sm_get_tx_fifo_level(PIX_PIO, PIX_SM) < 6;
}

// Test for empty transmit FIFO.
static inline bool pix_fifo_empty(void)
{
    return pio_sm_is_tx_fifo_empty(PIX_PIO, PIX_SM);
}

// Unconditionally attempt to send a PIX message.
// Meant for use with pix_ready() to fill the FIFO in a task handler.
static inline void pix_send(uint8_t dev3, uint8_t ch4, uint8_t byte, uint16_t word)
{
    assert(ch4 < 16);
    pio_sm_put(PIX_PIO, PIX_SM, PIX_MESSAGE(dev3, ch4, byte, word));
}

// Send a single PIX message, block if necessary. Normally, blocking is bad, but
// this unblocks so fast that it's not a problem for a few messages.
static inline void pix_send_blocking(uint8_t dev3, uint8_t ch4, uint8_t byte, uint16_t word)
{
    while (!pix_ready())
        tight_loop_contents();
    pix_send(dev3, ch4, byte, word);
}

#endif /* _PIX_H_ */
