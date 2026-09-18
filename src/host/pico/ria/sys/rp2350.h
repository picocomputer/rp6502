/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_RP2350_H_
#define _RIA_SYS_RP2350_H_

/* One user reported 280 MHz on the stock 1.10V; this is the rate the machine
 * is tested at. https://forums.raspberrypi.com/viewtopic.php?t=375975 */
#define SYS_RP2350_KHZ 256000

void rp2350_init(void);

/* RP2350_DRIVER comes first in the driver list, because later inits compute
 * clock dividers from the system clock. */
#define RP2350_DRIVER DRIVER(rp2350_init, nul_task, nul_task, nul_run, \
    nul_stop, nul_break, nul_config, nul_config, nul_sst)

#endif /* _RIA_SYS_RP2350_H_ */
