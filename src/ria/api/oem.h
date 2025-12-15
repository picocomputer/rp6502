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

// Attempt to change the code page.
// On failure, preserve current value if possible.
// Use default as a last resort.
uint16_t oem_set_code_page(uint16_t cp);

// Get the current code page which may be different than the
// setting if an application requested a temporary code page.
uint16_t oem_get_code_page(void);

// API set or query the code page.
bool oem_api_code_page(void);

// Only the code page specified by RP6502_CODE_PAGE is installed to flash.
// To include all code pages, set RP6502_CODE_PAGE to 0 (CMmakeLists.txt).
// This is the default for when RP6502_CODE_PAGE == 0.
#define OEM_DEFAULT_CODE_PAGE 437

#endif /* _RIA_API_OEM_H_ */
