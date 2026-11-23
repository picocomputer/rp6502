/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/vtmouse.h"
#include "core/hid/vtkey.h"
#include "core/term/term.h"
#include <stdbool.h>

/* The positions the X10 form and UTF-8's carry, from 0: this one and any
 * beyond are a byte 0, as xterm's MOUSE_LIMIT and EXT_MOUSE_LIMIT. */
#define VTMOUSE_X10_LIMIT (255 - 32)
#define VTMOUSE_UTF8_LIMIT (2047 - 32)

/* A value below 128 as one byte, or one of up to 2047 as two of UTF-8. */
static uint8_t vtmouse_utf8(char *out, uint8_t n, unsigned value)
{
    if (value < 0x80)
        out[n++] = (char)value;
    else
    {
        out[n++] = (char)(0xC0 | value >> 6);
        out[n++] = (char)(0x80 | (value & 0x3F));
    }
    return n;
}

/* A number in decimal. */
static uint8_t vtmouse_decimal(char *out, uint8_t n, unsigned value)
{
    char digits[5];
    uint8_t len = 0;
    do
        digits[len++] = (char)('0' + value % 10);
    while (value /= 10);
    while (len)
        out[n++] = digits[--len];
    return n;
}

/* A column or a row, from 0. */
static uint8_t vtmouse_position(char *out, uint8_t n, uint8_t form, unsigned value)
{
    switch (form)
    {
    case TERM_MOUSE_FORM_X10:
        out[n++] = value >= VTMOUSE_X10_LIMIT ? 0 : (char)(' ' + value + 1);
        return n;
    case TERM_MOUSE_FORM_UTF8:
        if (value >= VTMOUSE_UTF8_LIMIT)
        {
            out[n++] = 0;
            return n;
        }
        return vtmouse_utf8(out, n, ' ' + value + 1);
    default:
        out[n++] = ';';
        return vtmouse_decimal(out, n, value + 1);
    }
}

uint8_t vtmouse_encode(char out[VTMOUSE_MAX], uint8_t form, uint8_t event, uint8_t button,
                       uint8_t mods, uint16_t col, uint16_t row, uint16_t x, uint16_t y)
{
    const bool sgr = form == TERM_MOUSE_FORM_SGR || form == TERM_MOUSE_FORM_PIXELS;
    char final = 'M';
    unsigned code;
    if (event == VTMOUSE_X10)
    {
        if (button > VTMOUSE_RIGHT)
            return 0;
        code = ' ' + button;
    }
    else
    {
        /* Bits 1:0 the button, 3 for a release in the forms that do not
         * name it, 4:2 the modifiers, 5 motion, 6 the wheel's and 7 the
         * buttons from 8. A release of anything but the wheel's up and
         * down is SGR's 'm'. */
        code = ' ' + ((mods & VTKEY_MODIFIERS) << 2);
        if (event == VTMOUSE_MOTION)
            code += 32;
        const bool named = button < VTMOUSE_NONE || button > VTMOUSE_WHEEL_DOWN;
        if (event == VTMOUSE_RELEASE && named && sgr)
            final = 'm';
        if (event == VTMOUSE_RELEASE && named && !sgr)
            code += 3;
        else
            code += (button & 3u) + (button & 4u ? 64 : 0) + (button & 8u ? 128 : 0);
    }
    uint8_t n = 0;
    out[n++] = '\33';
    out[n++] = '[';
    if (sgr)
        out[n++] = '<';
    else if (form != TERM_MOUSE_FORM_URXVT)
        out[n++] = 'M';
    switch (form)
    {
    case TERM_MOUSE_FORM_X10:
        out[n++] = (char)code;
        break;
    case TERM_MOUSE_FORM_UTF8:
        n = vtmouse_utf8(out, n, code);
        break;
    case TERM_MOUSE_FORM_URXVT:
        n = vtmouse_decimal(out, n, code);
        break;
    default:
        n = vtmouse_decimal(out, n, code - ' ');
        break;
    }
    if (form == TERM_MOUSE_FORM_PIXELS)
    {
        col = x;
        row = y;
    }
    n = vtmouse_position(out, n, form, col);
    n = vtmouse_position(out, n, form, row);
    if (sgr || form == TERM_MOUSE_FORM_URXVT)
        out[n++] = final;
    return n;
}
