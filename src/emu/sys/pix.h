/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_SYS_PIX_H_
#define _EMU_SYS_PIX_H_

#include <stdbool.h>
#include <stdint.h>

#define PIX_DEVICE_XRAM 0
#define PIX_DEVICE_RIA 0
#define PIX_DEVICE_VGA 1
#define PIX_DEVICE_IDLE 7

bool pix_api_xreg(void); /* op 0x01: set XREGs off the xstack */

/* PIX bus send. No FIFO in the emu, so both commit immediately (blocking). */
bool pix_send(uint8_t dev, uint8_t channel, uint8_t byte, uint16_t word);
bool pix_send_blocking(uint8_t dev, uint8_t channel, uint8_t byte, uint16_t word);

static inline bool pix_ready(void)
{
    return true;
}

#endif /* _EMU_SYS_PIX_H_ */
