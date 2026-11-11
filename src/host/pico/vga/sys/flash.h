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

bool flash_program_request(uint16_t page);
void flash_task(void);

#define FLASH_DRIVER DRIVER(TASK_PUMPED(flash_task))

#endif /* _VGA_SYS_FLASH_H_ */
