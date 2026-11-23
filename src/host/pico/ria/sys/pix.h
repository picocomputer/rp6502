/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_PIX_H_
#define _RIA_SYS_PIX_H_

/* Pico Information eXchange bus driver.
 */

#include "core/sys/pix.h"

#include "machine.h"
#include <hardware/pio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define PIX_PIN_BASE 0 /* PIX0-PIX3 */
#define PIX_PIN_COUNT 4
#define PIX_PIO pio1
#define PIX_SM 1        /* fed by the cores */
#define PIX_STREAM_SM 3 /* fed by DMA from the stream ring */

/* Main events
 */

void pix_init(void);
void pix_stop(void);
void pix_reclock(uint16_t clkdiv_int, uint8_t clkdiv_frac);

// Room for at least one more message in PIX_SM's FIFO, and the VGA's gate
// open (vga.h): every message of the main loop passes it.
bool pix_ready(void);

// Bit 28 always 1, bits [31:29] for device id, etc.
#define PIX_MESSAGE(dev, ch, byte, word) \
    (0x10000000u | (dev << 29u) | (ch << 24) | ((byte) << 16) | (word))

// Send a PIX message once pix_ready(), blocking until then. The only way the
// main loop sends: a caller that must not block checks pix_ready() first.
void pix_send_blocking(uint8_t dev3, uint8_t ch4, uint8_t byte, uint16_t word);

#define PIX_DRIVER DRIVER(INIT(pix_init) STOP(pix_stop))

#endif /* _RIA_SYS_PIX_H_ */
