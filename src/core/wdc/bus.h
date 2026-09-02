/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The 6502's bus on a software machine: the CPU, the 6522, the RIA's
 * registers and the RAM, all advancing on one PHI2 enable. The 6502 is the
 * only master, so running the bus is running the machine.
 */

#ifndef _CORE_WDC_BUS_H_
#define _CORE_WDC_BUS_H_

#include "core/rp2350.h" /* SYS_RP2350_KHZ */

/* The system clock is counted oversampled so that a PHI2 period is a whole
 * number of ticks at rates whose divider is not: 2048 ticks per microsecond,
 * 2048000 per millisecond, against a rate in cycles per millisecond. */
#define SYS_OVERSAMPLE 8
#define SYS_TICKS_PER_US (SYS_RP2350_KHZ * SYS_OVERSAMPLE / 1000) /* 2048 */

/* Reset, from resb_assert: the parked bus is what the next cycle drives, and
 * a finished program's last access must not carry into a fresh one. The clock
 * is not reset -- it is machine uptime, and it rides through a restart. */
void bus_reset(void);

/* Run the bus up to the beam -- and with it every device on it. Bounded by
 * construction: vga_task advances at most one scanline per pass. */
void bus_task(void);

/* This driver's row in a machine's driver list; see core/driver.h. After VGA,
 * whose beam is the deadline this runs up to. */
#define BUS_DRIVER DRIVER(nul_init, bus_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config)

#endif /* _CORE_WDC_BUS_H_ */
