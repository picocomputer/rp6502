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

#include <stdbool.h>
#include <stdint.h>

void font_init(void);
/* The store rebuilt from the code page, for a restore. */
void font_restore(void);

void font_set_code_page(uint16_t cp);
uint16_t font_get_code_page(void);

/* A program asking for a page the asset does not carry is answered with
 * the page still in force. */
bool font_has_code_page(uint16_t cp);

#endif /* _FPGA_SW_FONT_H_ */
