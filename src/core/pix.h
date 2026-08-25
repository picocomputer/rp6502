/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Writing XRAM. On a Pico this crosses the PIX bus to the VGA's copy and is a
 * PIO FIFO write; elsewhere there is one XRAM and it lands directly. */

#ifndef _CORE_PIX_H_
#define _CORE_PIX_H_

#include <stdbool.h>
#include <stdint.h>

/* Well known PIX devices. 2-6 are for user expansion. Device 0 is the
 * RIA's own, virtual: it never reaches a bus. */
#define PIX_DEVICE_XRAM 0
#define PIX_DEVICE_RIA 0
#define PIX_DEVICE_VGA 1
#define PIX_DEVICE_IDLE 7

/* Op 0x01: the xreg burst off the xstack, to a device and channel. Every
 * machine implements it -- over the bus on a Pico, as a call on the two
 * that have one XRAM and one of everything else. */
bool pix_api_xreg(void);

/* Where a message goes once the two virtual devices are ruled out, for the
 * machines whose bus is a function call (core/pix.c). Returns the device's
 * ACK/NAK, true where there is nothing to reach. A machine with a real bus
 * implements pix_api_xreg itself and never defines this. */
bool pix_deliver(uint8_t dev, uint8_t channel, uint8_t byte, uint16_t word);

/* Room for at least one more. False means try again later: on a Pico the FIFO
 * is finite and std_task retires its forwarding count through this. */
bool pix_ready(void);

// One XRAM byte, to every copy of XRAM the machine has.
void pix_send_xram(uint16_t addr, uint8_t data);

#endif /* _CORE_PIX_H_ */
