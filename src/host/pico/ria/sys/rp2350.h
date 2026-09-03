/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The part this firmware runs on, brought up to the rate it is tested at.
 *
 * The VGA firmware is an RP2350 too and does not use this: it takes its clock
 * from whatever video mode it is about to run.
 */

#ifndef _RIA_SYS_RP2350_H_
#define _RIA_SYS_RP2350_H_

/* Read by whatever divides down from it -- the PIO clocks that make PHI2, the
 * PWM wrap that makes an audio sample rate, the radio's band select. The
 * voltage that lets it run this fast is this driver's own business.
 *
 * One user reported 280 MHz on the stock 1.10V; this is the rate the machine
 * is tested at. https://forums.raspberrypi.com/viewtopic.php?t=375975 */
#define SYS_RP2350_KHZ 256000

void rp2350_init(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. It goes
 * first: every other driver sets up something divided from this clock. */
#define RP2350_DRIVER DRIVER(rp2350_init, nul_task, nul_task, nul_run, \
    nul_stop, nul_break, nul_config, nul_config)

#endif /* _RIA_SYS_RP2350_H_ */
