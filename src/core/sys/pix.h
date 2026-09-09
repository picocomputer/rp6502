/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Writing XRAM. On a Pico this crosses the PIX bus to the VGA's copy and is a
 * PIO FIFO write; elsewhere there is one XRAM and it lands directly. */

#ifndef _CORE_SYS_PIX_H_
#define _CORE_SYS_PIX_H_

#include <stdbool.h>
#include <stdint.h>

/* A message carries three device bits, so it can name eight. Device 0 is an
 * XRAM write, which the VGA's own state machine takes; it is also the xreg
 * address of the RIA itself, which pix_api_xreg answers in place rather than
 * sending; 2 to 6 are for user expansion; device 7 is the idle message the bus
 * resynchronizes on. */
#define PIX_DEVICE_XRAM 0
#define PIX_DEVICE_RIA 0
#define PIX_DEVICE_VGA 1
#define PIX_DEVICE_IDLE 7

/* Op 0x01: the xreg burst off the xstack, to a device and channel. */
bool pix_api_xreg(void);

/* Room for at least one more message. A Pico's FIFO is finite and answers
 * false when it is nearly full, so std_task retires its forwarding count
 * through this. */
bool pix_ready(void);

void pix_send_xram(uint16_t addr, uint8_t data);

#endif /* _CORE_SYS_PIX_H_ */
