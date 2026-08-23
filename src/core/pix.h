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

/* Room for at least one more. False means try again later: on a Pico the FIFO
 * is finite and std_task retires its forwarding count through this. */
bool pix_ready(void);

// One XRAM byte, to every copy of XRAM the machine has.
void pix_send_xram(uint16_t addr, uint8_t data);

#endif /* _CORE_PIX_H_ */
