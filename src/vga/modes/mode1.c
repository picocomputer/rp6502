/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "modes/mode1.h"
#include "pico/scanvideo.h"

typedef struct
{
    int16_t xpos_px;
    int16_t ypos_px;
    int16_t width_chars;
    int16_t height_chars;
    uint16_t xram_data_ptr;
    uint16_t xram_color_ptr;
    uint16_t xram_font_ptr;
} mode1_config_t;

typedef struct
{
    uint8_t font_code;
    uint8_t attributes;
    uint16_t fg_color;
    uint16_t bg_color;
} mode1_16_data_t;

bool mode1_prog(uint16_t *xregs)
{
    return false;
}
