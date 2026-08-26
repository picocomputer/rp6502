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

/* The very first thing main() does: raise the voltage and set the system clock. */
void sys_main(void);

void sys_init(void);

/* Monitor commands
 */

void sys_mon_reboot(const char *args);
void sys_mon_reset(const char *args);
void sys_mon_status(const char *args);

#endif /* _RIA_SYS_SYS_H_ */
