/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_TAB_H_
#define _CORE_HID_TAB_H_

/* HID absolute-pointer ("tablet") driver. Reports an absolute canvas position
 * instead of relative motion: a relative mouse is integrated and clamped to the
 * canvas, an absolute digitizer/pen is scaled to it. The multi-byte coordinates
 * are delivered coherently through byte-wide XRAM by a unary window encoding (no
 * RIA/act_loop help). See tab.c for the XRAM contract.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "core/hid/hid.h"

#define TAB_MAX_CONTACTS 8 /* fixed slot count; the ROM allocates the whole block */
#define TAB_HEADER_SIZE 4  /* control, status, wheel, pan */
#define TAB_CONTACT_SIZE 6 /* flags, x0, x1, x2, y0, y1 */
#define TAB_BLOCK_SIZE (TAB_HEADER_SIZE + TAB_MAX_CONTACTS * TAB_CONTACT_SIZE)

/* Header offsets */
#define TAB_OFF_CONTROL 0
#define TAB_OFF_STATUS 1
#define TAB_OFF_WHEEL 2
#define TAB_OFF_PAN 3
#define TAB_OFF_CONTACTS 4

/* status (fw->ROM). A machine with a host cursor to lend says so; a Pico
 * has none and never sets it. */
#define TAB_STATUS_HOST_CURSOR 0x01

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

/* Main events
 */

void tab_init(void);
void tab_stop(void);

// Set the extended register value.
bool tab_xreg(uint16_t word);

// Whether a program has asked for this device's block.
bool tab_is_mapped(void);

// Parse HID report descriptor for an absolute or relative pointer.
bool tab_mount(int slot, const hid_report_map_t *map);

// Clean up descriptor when device is disconnected.
bool tab_umount(int slot);

// Process HID report.
void tab_report(int slot, uint8_t const *report, size_t size);

/* A host whose OS decodes its own pointer, in place of a report. It gives
 * canvas pixels, which is what the block carries, so nothing is scaled.
 */

/* A hovering absolute pointer (mouse/pen) at canvas x,y with a button bitmap
 * (TAB_FLAG_*). Occupies contact 0 and declares a host cursor is available. */
void tab_host_pointer(int x, int y, uint8_t buttons);

/* n touch contacts (tip down, no hover); the rest go inactive. No host cursor. */
void tab_host_touch(const tab_point_t *pts, int n);

/* Accumulate host scroll into the wheel/pan bytes (8-bit wrapping counters),
 * mirroring the mouse. */
void tab_host_wheel(int dwheel, int dpan);

/* The pointer left the window / all contacts released: everything inactive. */
void tab_host_clear(void);

/* The ROM's requested cursor shape (control byte, one of TAB_CURSOR_*); 0 when
 * unmapped. */
uint8_t tab_control(void);

#endif /* _CORE_HID_TAB_H_ */
