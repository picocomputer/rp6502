/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _XMIRROR_H_
#define _XMIRROR_H_

/* A real xterm, given everything the console's terminal is given, so a
 * screen of the console can be held against xterm's. xterm runs on a
 * display of its own, an Xvfb started for it, attached to a pseudoterminal
 * that only the mirror writes. Its replies go nowhere: the program being
 * run hears the console's.
 *
 * xterm prints its screen on request (MC, CSI i) to a file, with the
 * attributes and colors of each cell, which is read back into an
 * xm_screen_t. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XM_MAX_ROWS 64
#define XM_MAX_COLS 160

/* A color: an entry of the 256, or XM_DEFAULT_FG or XM_DEFAULT_BG, with
 * XM_FAINT for SGR 2; or XM_RGB and the red, green and blue at 5 bits each. */
#define XM_DEFAULT_FG 256u
#define XM_DEFAULT_BG 257u
#define XM_FAINT 0x400u
#define XM_RGB 0x1000000u
#define XM_RGB5(r, g, b) (XM_RGB | ((uint32_t)(r) << 10) | ((uint32_t)(g) << 5) | (uint32_t)(b))

#define XM_ATTR_UL 1u
#define XM_ATTR_BLINK 2u
#define XM_ATTR_STRIKE 4u
#define XM_ATTR_ITALIC 8u
#define XM_ATTR_OVERLINE 16u
#define XM_ATTR_DUL 32u /* the underline is double */

/* The line sizes, as ESC # n: 5 normal, 6 wide, 3 top half, 4 bottom half. */
typedef struct
{
    uint32_t cp; /* Unicode */
    uint32_t fg, bg;
    uint8_t attr;
    bool known; /* false past the end of a printed line: only its text is */
} xm_cell_t;

typedef struct
{
    int rows, cols;
    xm_cell_t cell[XM_MAX_ROWS][XM_MAX_COLS];
    uint8_t line[XM_MAX_ROWS]; /* 3, 4, 5 or 6, or 0 for not known */
    int cursor_x, cursor_y;    /* from 0 */
    bool reverse;              /* DECSCNM */
} xm_screen_t;

/* Starts Xvfb and xterm, rows by cols, printing into dir. False with why on
 * stderr if either is missing or will not start. */
bool xm_start(int rows, int cols, const char *dir);

/* Bytes for xterm, as the console's terminal gets them. */
void xm_write(const char *buf, size_t n);

/* xterm's screen now, after everything written to it up to the last whole
 * sequence. False if xterm does not answer. */
bool xm_snap(xm_screen_t *s);

void xm_stop(void);

#endif /* _XMIRROR_H_ */
