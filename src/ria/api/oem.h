/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _OEM_H_
#define _OEM_H_

#include <stdint.h>

void oem_init(void);
void oem_api_codepage();
uint16_t oem_validate_code_page(uint16_t cp);

#endif /* _OEM_H_ */
