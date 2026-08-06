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

void font_set_code_page(uint16_t cp);
uint16_t font_get_code_page(void);

/* Whether the asset carries a page, which is the API's question and not
 * the store's: a program that asks for one that is not here is answered
 * with the page still in force, the way f_setcp's refusal answers the
 * same request on the RIA. The seventeen are the same seventeen. */
bool font_has_code_page(uint16_t cp);

#endif /* _FPGA_SW_FONT_H_ */
