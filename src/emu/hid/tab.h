/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_HID_TAB_H_
#define _EMU_HID_TAB_H_

#include "ria/hid/tab.h"

/* Absolute pointer ("tablet") device. Unlike the relative mouse it reports an
 * absolute canvas position. The multi-byte X/Y are delivered coherently through
 * byte-wide XRAM without any RIA/act_loop help: each coordinate is a set of
 * single-byte "windows", exactly one non-zero, decoded first-non-zero-wins. A
 * coordinate past the canvas (X>639) marks an inactive contact. See ria/hid/tab.c
 * for the shared contract; this mirror is driven by host input instead of USB. */

#define TAB_MAX_CONTACTS 8    /* fixed slot count; the ROM allocates the whole block */
#define TAB_HEADER_SIZE 2     /* status, control */
#define TAB_CONTACT_SIZE 6    /* flags, x0, x1, x2, y0, y1 */
#define TAB_BLOCK_SIZE (TAB_HEADER_SIZE + TAB_MAX_CONTACTS * TAB_CONTACT_SIZE)

/* Header offsets */
#define TAB_OFF_STATUS 0
#define TAB_OFF_CONTROL 1
#define TAB_OFF_CONTACTS 2

/* status (fw->ROM) */
#define TAB_STATUS_HOST_CURSOR 0x01 /* host will render a cursor if asked (see tab.h contract) */

/* contact flags (b7 hover so the 6502 tests it with BIT/BMI) */
#define TAB_FLAG_LEFT 0x01
#define TAB_FLAG_RIGHT 0x02
#define TAB_FLAG_MIDDLE 0x04
#define TAB_FLAG_BTN4 0x08
#define TAB_FLAG_BTN5 0x10
#define TAB_FLAG_HOVER 0x80

/* control (ROM->fw): the fixed universal cursor enum every backend renders. */
enum
{
    TAB_CURSOR_OFF = 0, /* host cursor hidden; the ROM draws its own */
    TAB_CURSOR_ARROW,
    TAB_CURSOR_CROSSHAIR,
    TAB_CURSOR_IBEAM,
    TAB_CURSOR_HAND,
    TAB_CURSOR_RESIZE_EW,
    TAB_CURSOR_RESIZE_NS,
    TAB_CURSOR_COUNT,
};

typedef struct
{
    int16_t x, y; /* canvas pixels */
} tab_point_t;

/* xreg_ria_tablet API: point the report block (TAB_BLOCK_SIZE bytes) at an XRAM
 * address (0xFFFF = off). Mirrors ria/hid/tab.c tab_xreg. */
bool tab_set_xram(uint16_t addr);

/* True once a program has pointed the report block at XRAM (xreg_ria_tablet). */
bool tab_is_mapped(void);

/* A hovering absolute pointer (mouse/pen) at canvas x,y with a button bitmap
 * (TAB_FLAG_*). Occupies contact 0 and declares a host cursor is available. */
void tab_host_pointer(int x, int y, uint8_t buttons);

/* n touch contacts (tip down, no hover); the rest go inactive. No host cursor. */
void tab_host_touch(const tab_point_t *pts, int n);

/* The pointer left the window / all contacts released: everything inactive. */
void tab_host_clear(void);

/* The ROM's requested cursor shape (control byte, one of TAB_CURSOR_*); 0 when
 * unmapped. */
uint8_t tab_control(void);

#endif /* _EMU_HID_TAB_H_ */
