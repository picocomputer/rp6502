/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_SYS_RESB_H_
#define _RIA_SYS_RESB_H_

#include "core/wdc/resb.h"
#include <stdint.h>

#define CPU_RESB_PIN 26

/* The minimum hold, in microseconds: two PHI2 cycles, rounded up. Public
 * because ria.c sizes the action watchdog with it. */
uint32_t resb_get_reset_us(void);

/* Restart the hold from now. From phi2.c, whose reclock invalidates it. */
void resb_reclock(void);

/* Raise the line once the hold has elapsed. A timer is per-pass work, which
 * is the one part of this that is a driver row. */
void resb_task(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. The line
 * itself is core/sys/sys.c's -- only the hold timer is a row. */
#define RESB_DRIVER DRIVER(nul_init, resb_task, nul_task, nul_run, nul_stop, nul_break, \
    nul_config, nul_config)

#endif /* _RIA_SYS_RESB_H_ */
