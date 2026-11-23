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

#endif /* _CORE_HID_VTMOUSE_H_ */
