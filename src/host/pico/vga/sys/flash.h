/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SYS_FLASH_H_
#define _VGA_SYS_FLASH_H_

/* Writing this board's own flash, which is how it is updated: the RIA streams
 * a sector into xram over PIX, then asks for it to be committed. */

#include "core/sys/driver.h"

#include <stdbool.h>
#include <stdint.h>

/* Queue a sector erase+program from xram. False when the index is past the
 * flash -- the SDK panics on that rather than refusing. */
bool flash_request(uint16_t sector_index);

/* Deferred, because the write blocks for tens of milliseconds. */
void flash_task(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define FLASH_DRIVER DRIVER(nul_init, flash_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config)

#endif /* _VGA_SYS_FLASH_H_ */
