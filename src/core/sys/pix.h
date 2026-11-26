/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Writing XRAM. On a Pico this crosses the PIX bus to the VGA's copy;
 * elsewhere there is one XRAM and it lands directly. */

#ifndef _CORE_SYS_PIX_H_
#define _CORE_SYS_PIX_H_

#include <stdbool.h>
#include <stdint.h>

/* A message carries three device bits, so it can name eight. Device 0 is an
 * XRAM write, which the VGA's own state machine takes; it is also the xreg
 * device of the RIA itself, which pix_api_xreg answers in place rather than
 * sending; 2 to 6 are for user expansion; device 7 is the idle message the bus
 * resynchronizes on. */
#define PIX_DEVICE_XRAM 0
#define PIX_DEVICE_RIA 0
#define PIX_DEVICE_VGA 1
#define PIX_DEVICE_IDLE 7

/* The key to the flash of a Pico's VGA: the page of XRAM after the one that
 * FLASH_PROGRAM writes from, filled with the 32-bit word "FLSH". The RIA
 * fills it before the first FLASH_PROGRAM, and the VGA clears it and the
 * page before it at REBOOT_OR_LOCKUP, so a stray FLASH_PROGRAM finds no key. */
#define PIX_FLASH_KEY 0x48534C46u /* "FLSH", least significant byte first */
#define PIX_FLASH_KEY_ADDR 0x0100
#define PIX_FLASH_KEY_SIZE 0x0100

/* Op 0x01: the xreg burst off the xstack, to a device and channel. */
bool pix_api_xreg(void);

/* Queues xram[addr, end) for the video device's copy, as much as there is
 * room for, and returns the address it queued up to. */
uint32_t pix_stream_xram(uint32_t addr, uint32_t end);

/* True once everything pix_stream_xram queued goes out ahead of any message
 * sent after this returns. */
bool pix_stream_done(void);

#endif /* _CORE_SYS_PIX_H_ */
