/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host shim for ria/sys/pix.h. The emulator's VGA renders from the same
 * xram[] the RIA writes, so the PIX mirror the firmware pumps in std_task
 * is already satisfied: the FIFO is always ready and sends are no-ops.
 */

#ifndef _EMU_SHIM_SYS_PIX_H_
#define _EMU_SHIM_SYS_PIX_H_

#include <stdint.h>
#include <stdbool.h>

#define PIX_DEVICE_XRAM 0
#define PIX_DEVICE_RIA 0
#define PIX_DEVICE_VGA 1
#define PIX_DEVICE_IDLE 7

static inline bool pix_ready(void)
{
    return true;
}

static inline void pix_send(uint8_t dev3, uint8_t ch4, uint8_t byte, uint16_t word)
{
    (void)dev3, (void)ch4, (void)byte, (void)word;
}

#endif /* _EMU_SHIM_SYS_PIX_H_ */
