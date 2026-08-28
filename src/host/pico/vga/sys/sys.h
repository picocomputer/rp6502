/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SYS_SYS_H_
#define _VGA_SYS_SYS_H_

#include "core/mach.h"

/* System Information
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// VGA version string
const char *sys_version(void);

void sys_task(void);
/* Queue a sector erase+program from xram. False when the index is past the
 * flash -- the SDK panics on that rather than refusing. */
bool sys_flash_request(uint16_t sector_index);

/* This driver's row in a machine's driver list; see core/mach.h. */
#define SYS_DRIVER DRIVER(nul_init, sys_task, nul_task, nul_run, nul_stop, nul_break)

#endif /* _VGA_SYS_SYS_H_ */
