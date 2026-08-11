/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_FONT_H_
#define _FPGA_SW_FONT_H_

#include <stdbool.h>
#include <stdint.h>

void font_init(void);
void font_restore(void);

void font_set_code_page(uint16_t cp);
uint16_t font_get_code_page(void);

bool font_has_code_page(uint16_t cp);

#endif
