/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_FONT_H_
#define _CORE_TERM_FONT_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

extern uint8_t font8[2048];
extern uint8_t font16[4096];
extern uint8_t font_dec_8[8 * 32];
extern uint8_t font_dec_16[16 * 32];
extern uint8_t italic16[16 * 128];

/* Main events
 */

void font_init(void);

void font_set_code_page(uint16_t cp);

/* The same tables as font_set_code_page, without the terminal reset. A
 * savestate has already put the terminal's cells back by the time it
 * restores the code page, and a reset would erase them. */
void font_load_code_page(uint16_t cp);

uint16_t font_get_code_page(void);

#define FONT_DRIVER DRIVER(font_init, nul_task, nul_task, nul_run, nul_stop, nul_break, nul_config, nul_config, nul_sst)

#endif /* _CORE_TERM_FONT_H_ */
