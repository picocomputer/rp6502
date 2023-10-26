/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _OEM_H_
#define _OEM_H_

#include <stdint.h>

/* Kernel events
 */

void oem_init(void);

// Attempt to change the code page.
// On failure, preserve current value if possible.
// Use default as a last resort.
uint16_t oem_set_codepage(uint16_t cp);

// API query the codepage.
void oem_api_codepage(void);

#endif /* _OEM_H_ */
