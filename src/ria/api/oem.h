/*
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_OEM_H_
#define _RIA_API_OEM_H_

/* The OEM driver manages IBM/DOS style code pages.
 * The affects RP6502-VGA, FatFs, and keyboards.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void oem_init(void);
void oem_stop(void);

// API set or query the code page.
bool oem_api_code_page(void);

// Configuration setting KB
void oem_load_code_page(const char *str, size_t len);
bool oem_set_code_page(uint32_t cp);
uint16_t oem_get_code_page(void);

#endif /* _RIA_API_OEM_H_ */
