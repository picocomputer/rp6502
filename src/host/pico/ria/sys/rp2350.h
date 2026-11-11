/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_RP2350_H_
#define _RIA_SYS_RP2350_H_

#include <hardware/irq.h>

/* One user reported 280 MHz on the stock 1.10V; this is the rate the machine
 * is tested at. https://forums.raspberrypi.com/viewtopic.php?t=375975 */
#define SYS_RP2350_KHZ 256000

/* Interrupt priorities, highest first. A lower number preempts a higher one,
 * and the RP2350 tells them apart in steps of 0x10. Each handler is in RAM. */
#define RP2350_IRQ_PRIORITY_VGA (PICO_DEFAULT_IRQ_PRIORITY - 0x20)
#define RP2350_IRQ_PRIORITY_AUD (PICO_DEFAULT_IRQ_PRIORITY - 0x10)
#define RP2350_IRQ_PRIORITY_USB PICO_DEFAULT_IRQ_PRIORITY
#define RP2350_IRQ_PRIORITY_CYW (PICO_DEFAULT_IRQ_PRIORITY + 0x10)

void rp2350_init(void);

/* RP2350_DRIVER comes first in the driver list, because later inits compute
 * clock dividers from the system clock. */
#define RP2350_DRIVER DRIVER(INIT(rp2350_init))

#endif /* _RIA_SYS_RP2350_H_ */
