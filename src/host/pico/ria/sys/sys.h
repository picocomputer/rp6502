/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_SYS_H_
#define _RIA_SYS_SYS_H_

/* System monitor commands.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "core/sys.h" /* SYS_RP2350_KHZ, the rate every machine answers to */

// The boost that rate is tested at, which only the board that sets it needs.
#define SYS_RP2350_VREG VREG_VOLTAGE_1_15

/* Main events
 */

/* The voltage and the system clock, which everything derived from them needs
 * first: the UART's baud, the PIO dividers, the radio's band. */
void sys_init(void);


/* Queue what this machine is, for the monitor's boot banner. */
void sys_add_boot_response(void);

/* Monitor commands
 */

void sys_mon_reboot(const char *args);
void sys_mon_reset(const char *args);
void sys_mon_status(const char *args);

/* This driver's machine-lifecycle row; see core/lifecycle.h. First, because the clock
 * it sets is what every later row is timed against. */
#define SYS_MACH_LIFECYCLE LIFECYCLE(sys_init, nul_task, nul_task, nul_run, nul_stop, nul_break)

#endif /* _RIA_SYS_SYS_H_ */
