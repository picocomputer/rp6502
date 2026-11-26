/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SYS_FLASH_H_
#define _VGA_SYS_FLASH_H_

#include "core/sys/driver.h"

#include <stdbool.h>
#include <stdint.h>

/* FLASH_PROGRAM: XRAM $0000-$00FF goes to the page, once flash_task runs.
 * False when the page is refused. */
bool flash_program_request(uint16_t page);

/* Clears the page FLASH_PROGRAM writes from and the key after it. */
void flash_lock(void);

void flash_task(void);

#define FLASH_DRIVER DRIVER(TASK_PUMPED(flash_task))

#endif /* _VGA_SYS_FLASH_H_ */
