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

/* The code page the fonts give for cp: cp itself when the fonts have it,
 * otherwise 0, ASCII alone. Choosing a different page resets the terminal,
 * so every machine with the fonts, and the RIA following the VGA's terminal,
 * work the page out with this. */
static inline uint16_t font_code_page_of(uint16_t cp)
{
    switch (cp)
    {
    case 437:
    case 720:
    case 737:
    case 771:
    case 775:
    case 850:
    case 852:
    case 855:
    case 857:
    case 860:
    case 861:
    case 862:
    case 863:
    case 864:
    case 865:
    case 866:
    case 869:
        return cp;
    default:
        return 0;
    }
}

void font_set_code_page(uint16_t cp);

/* The same tables as font_set_code_page, without the terminal reset. A
 * savestate has already put the terminal's cells back by the time it
 * restores the code page, and a reset would erase them. */
void font_load_code_page(uint16_t cp);

uint16_t font_get_code_page(void);

#define FONT_DRIVER DRIVER(INIT(font_init))

#endif /* _CORE_TERM_FONT_H_ */
