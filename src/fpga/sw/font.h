/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The VGA chip's font.c without the tables: here the glyphs live in the
 * video device and the soft CPU only moves them, so there is nothing to
 * export but the lifecycle and the setting.
 */

#ifndef _FPGA_SW_FONT_H_
#define _FPGA_SW_FONT_H_

#include <stdint.h>

void font_init(void);

void font_set_code_page(uint16_t cp);
uint16_t font_get_code_page(void);

#endif /* _FPGA_SW_FONT_H_ */
