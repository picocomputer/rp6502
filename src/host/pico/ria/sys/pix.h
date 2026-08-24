/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_PIX_H_
#define _RIA_SYS_PIX_H_

/* Pico Information eXchange bus driver.
 */

#include "core/pix.h"

#include <hardware/pio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define PIX_PIN_BASE 0 /* PIX0-PIX3 */
#define PIX_PIN_COUNT 4
#define PIX_PIO pio1
#define PIX_SM 1

/* Main events
 */

void pix_init(void);
void pix_stop(void);
void pix_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac);

/* API to set XREGs
 */

void pix_ack(void);
void pix_nak(void);

// External access to PIX VGA response. Used by uf2 flash tool.
void pix_wait_begin(uint32_t timeout_ms);
// 1=ack, -1=nak, -2=timeout, 0=still waiting. Resets state on terminal.
int pix_wait_poll(void);

// Bit 28 always 1, bits [31:29] for device id, etc.
#define PIX_MESSAGE(dev, ch, byte, word) \
    (0x10000000u | (dev << 29u) | (ch << 24) | ((byte) << 16) | (word))

// Macro for the RIA. Use the inline functions elsewhere.
#define PIX_SEND_XRAM(addr, data) \
    PIX_PIO->txf[PIX_SM] = (PIX_MESSAGE(PIX_DEVICE_XRAM, 0, (data), (addr)))

// Unconditionally attempt to send a PIX message.
// Meant for use with pix_ready() to fill the FIFO in a task handler.
static inline void pix_send(uint8_t dev3, uint8_t ch4, uint8_t byte, uint16_t word)
{
    assert(dev3 < 8);
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

#endif /* _RIA_SYS_PIX_H_ */
