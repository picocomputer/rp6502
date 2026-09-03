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

#include <stdint.h>

/* 6502 cycles this machine has run, ever. The clock above is the beam's and
 * runs whether or not the CPU does, so it cannot answer this: a machine held
 * in reset still ages. Nothing in the machine reads it -- it is here so a test
 * can say how many cycles a frame was worth, which is otherwise unobservable
 * from outside. */
uint64_t bus_cycles(void);

/* Reset, from resb_assert: the parked bus is what the next cycle drives, and
 * a finished program's last access must not carry into a fresh one. The clock
 * is not reset -- it is machine uptime, and it rides through a restart. */
void bus_reset(void);

/* Run the bus up to the beam -- and with it every device on it. Bounded by
 * construction: vga_task advances at most one scanline per pass. */
void bus_task(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. After VGA,
 * whose beam is the deadline this runs up to. */
#define BUS_DRIVER DRIVER(nul_init, bus_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config)

#endif /* _CORE_WDC_BUS_H_ */
