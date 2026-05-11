/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_TERM_FONT_H_
#define _VGA_TERM_FONT_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

extern uint8_t font8[2048];
extern uint8_t font16[4096];
extern uint8_t font_dec_8[8 * 32];
extern uint8_t font_dec_16[16 * 32];

/* Main events
 */

void font_init(void);

void font_set_code_page(uint16_t cp);

#endif /* _VGA_TERM_FONT_H_ */
