/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _RIA_API_OEM_H_
#define _RIA_API_OEM_H_

/* The OEM driver manages IBM/DOS style code pages.
 * This affects RP6502-VGA, FatFs, and keyboards.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void oem_init(void);
void oem_stop(void);

// Code page without saving to config
void oem_set_code_page_run(uint16_t cp);
uint16_t oem_get_code_page_run(void);

// Configuration setting CP
void oem_load_code_page(const char *str);
bool oem_set_code_page(uint32_t cp);
uint16_t oem_get_code_page(void);

#endif /* _RIA_API_OEM_H_ */
