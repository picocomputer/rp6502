/*
 * Copyright (c) 2023 Rumbledethumps
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

/* Kernel events
 */

void oem_init(void);
void oem_stop(void);

// Attempt to change the code page.
// On failure, preserve current value if possible.
// Use default as a last resort.
uint16_t oem_set_codepage(uint16_t cp);

// API set or query the codepage.
bool oem_api_codepage(void);

#endif /* _RIA_API_OEM_H_ */
