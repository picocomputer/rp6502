/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SYS_SYS_H_
#define _VGA_SYS_SYS_H_

/* System Information
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// VGA version string
const char *sys_version(void);

void sys_task(void);
void sys_flash_request(uint16_t sector_index);

#endif /* _VGA_SYS_SYS_H_ */
