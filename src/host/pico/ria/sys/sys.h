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

// We run the RP2350 at 256MHz with 0.05V boost.
// One user tested up to 280 MHz on the default 1.10V.
// https://forums.raspberrypi.com/viewtopic.php?t=375975
#define SYS_RP2350_KHZ 256000
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
