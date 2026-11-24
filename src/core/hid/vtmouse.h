/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What a mouse event sends to a program that asked for the mouse, as
 * xterm's button.c sends it (EditorButton): CSI M and three bytes, or the
 * numbers of SGR's, urxvt's or SGR's in pixels, in the form the console's
 * terminal is in (TERM_MOUSE_FORM_, term.h). Which events a program is
 * told of is the console's mouse's to decide (core/term/vterm_mouse.h). */

#ifndef _CORE_HID_VTMOUSE_H_
#define _CORE_HID_VTMOUSE_H_

#include <stdint.h>

/* The events. A press under X10's events (?9) is VTMOUSE_X10: the button
 * alone, with no modifiers. */
#define VTMOUSE_PRESS 0
#define VTMOUSE_RELEASE 1
#define VTMOUSE_MOTION 2
#define VTMOUSE_X10 3

/* The buttons, numbered as xterm numbers them after its DEC adjustment.
 * VTMOUSE_NONE is motion with no button held. The wheel's are presses
 * alone, one a notch. */
#define VTMOUSE_LEFT 0
#define VTMOUSE_MIDDLE 1
#define VTMOUSE_RIGHT 2
#define VTMOUSE_NONE 3
#define VTMOUSE_WHEEL_UP 4
#define VTMOUSE_WHEEL_DOWN 5
#define VTMOUSE_WHEEL_LEFT 6
#define VTMOUSE_WHEEL_RIGHT 7
#define VTMOUSE_BTN4 8
#define VTMOUSE_BTN5 9

/* The longest report: CSI < 255 ; 65536 ; 65536 M. */
#define VTMOUSE_MAX 19

/* A report, written with no NUL; returns its length, 0 for a button X10's
 * events do not carry. mods are vtkey.h's VTKEY_SHIFT, _ALT and _CTRL.
 * col and row are the cell, x and y the pixel, each from 0, and the pixel
 * form takes the pixel. The X10 form holds no more than column or row 222,
 * and UTF-8's no more than 2014: beyond is a byte 0, as xterm sends it. */
uint8_t vtmouse_encode(char out[VTMOUSE_MAX], uint8_t form, uint8_t event, uint8_t button,
                       uint8_t mods, uint16_t col, uint16_t row, uint16_t x, uint16_t y);

/* The longest report of highlight tracking: CSI < and six numbers of up to
 * three digits, the positions of the console's cells. */
#define VTMOUSE_HILITE_MAX 27

/* What highlight tracking (?1001) sends as its drag ends, as xterm's
 * EndExtend: with count 2, CSI t and the column and row where the
 * highlight ends; with count 6, CSI T and the columns and rows where it
 * starts, where it ends and where the pointer came up. pos holds them,
 * each column before its row, from 0. The X10 form and UTF-8's carry each
 * as a byte or two, and the others as decimal numbers, SGR's and the
 * pixel form's after a '<'. */
uint8_t vtmouse_hilite(char out[VTMOUSE_HILITE_MAX], uint8_t form, const uint8_t *pos,
                       uint8_t count);

/* The DEC locator's events (DECLRP), as xterm numbers them: a button going
 * down is VTMOUSE_LOCATOR_DOWN plus twice its locator number, and coming
 * up one more. */
#define VTMOUSE_LOCATOR_UNAVAILABLE 0 // outside the screen, or no pointer
#define VTMOUSE_LOCATOR_REQUEST 1     // the answer to DECRQLP
#define VTMOUSE_LOCATOR_DOWN 2
#define VTMOUSE_LOCATOR_OUTSIDE 10 // the locator left the filter rectangle

/* The buttons, a locator number each, and as bits of DECLRP's mask: the
 * left button is 0 and bit 2, the middle 1 and bit 1, the right 2 and bit
 * 0, and the fourth 3 and bit 3, as DEC has them. */
#define VTMOUSE_LOCATOR_LEFT 0
#define VTMOUSE_LOCATOR_MIDDLE 1
#define VTMOUSE_LOCATOR_RIGHT 2
#define VTMOUSE_LOCATOR_M4 3
#define VTMOUSE_LOCATOR_MASK(n) ((uint8_t)((n) == VTMOUSE_LOCATOR_M4 ? 8 : 4 >> (n)))

/* The longest DECLRP: CSI, an event, the mask, and a row and a column of up
 * to five digits each, ; between them, and & w. */
#define VTMOUSE_LOCATOR_MAX 21

/* A locator report, DECLRP: CSI event ; buttons ; row ; col & w, or
 * CSI 0 & w alone for VTMOUSE_LOCATOR_UNAVAILABLE. row and col are from 1,
 * and buttons is the mask of those held. */
uint8_t vtmouse_locator(char out[VTMOUSE_LOCATOR_MAX], uint8_t event, uint8_t buttons,
                        uint16_t row, uint16_t col);

#endif /* _CORE_HID_VTMOUSE_H_ */
