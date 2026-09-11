/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SYS_FLASH_H_
#define _VGA_SYS_FLASH_H_

/* Writing this board's own flash, which is how it is updated: the RIA stages a
 * page in xram over PIX, then asks for it to be programmed. */

#include "core/sys/driver.h"

#include <stdbool.h>
#include <stdint.h>

/* Queue a program of one page from the first 256 bytes of xram, erasing the
 * page's sector first when the page is not already blank. Deciding that means
 * reading the flash, so only the board holding it can decide, which is why the
 * RIA has no erase of its own to ask for. A page index reaches 16 MB, the most
 * a 16-bit PIX word carries. False when the index is past this board's flash,
 * because the SDK asserts on that and its assert does not compile out. */
bool flash_program_request(uint16_t page);

/* Deferred, because a program blocks for well under a millisecond but the
 * erase it may have to do first blocks for tens of them. */
void flash_task(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define FLASH_DRIVER DRIVER(nul_init, flash_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config, nul_sst)

#endif /* _VGA_SYS_FLASH_H_ */
