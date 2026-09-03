/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_TABLET_H_
#define _CORE_HID_TABLET_H_

/* HID absolute-pointer ("tablet") driver. Reports an absolute canvas position
 * instead of relative motion: a relative mouse is integrated and clamped to the
 * canvas, an absolute digitizer/pen is scaled to it. The multi-byte coordinates
 * are delivered coherently through byte-wide XRAM by a unary window encoding (no
 * RIA/act_loop help). See tablet.c for the XRAM contract.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "core/hid/hid.h"

#define TABLET_MAX_CONTACTS 8 /* fixed slot count; the ROM allocates the whole block */
#define TABLET_HEADER_SIZE 4  /* control, status, wheel, pan */
#define TABLET_CONTACT_SIZE 6 /* flags, x0, x1, x2, y0, y1 */
#define TABLET_BLOCK_SIZE (TABLET_HEADER_SIZE + TABLET_MAX_CONTACTS * TABLET_CONTACT_SIZE)

/* Header offsets */
#define TABLET_OFF_CONTROL 0
#define TABLET_OFF_STATUS 1
#define TABLET_OFF_WHEEL 2
#define TABLET_OFF_PAN 3
#define TABLET_OFF_CONTACTS 4

/* status (fw->ROM). A machine with a host cursor to lend says so; a Pico
 * has none and never sets it. */
#define TABLET_STATUS_HOST_CURSOR 0x01

/* contact flags (b7 hover so the 6502 tests it with BIT/BMI) */
#define TABLET_FLAG_LEFT 0x01
#define TABLET_FLAG_RIGHT 0x02
#define TABLET_FLAG_MIDDLE 0x04
#define TABLET_FLAG_BTN4 0x08
#define TABLET_FLAG_BTN5 0x10
#define TABLET_FLAG_HOVER 0x80

/* control (ROM->fw): the fixed universal cursor enum every backend renders. */
enum
{
    TABLET_CURSOR_OFF = 0, /* host cursor hidden; the ROM draws its own */
    TABLET_CURSOR_ARROW,
    TABLET_CURSOR_CROSSHAIR,
    TABLET_CURSOR_IBEAM,
    TABLET_CURSOR_HAND,
    TABLET_CURSOR_RESIZE_EW,
    TABLET_CURSOR_RESIZE_NS,
    TABLET_CURSOR_COUNT,
};

typedef struct
{
    int16_t x, y; /* canvas pixels */
} tablet_point_t;

/* Main events
 */

#define TABLET_MAX_MICE 4

/* One absolute or relative pointer, as the driver reads it. A descriptor
 * is parsed into this; a machine with no descriptor writes one out. */
typedef struct tablet_connection
{
    bool valid;
    int slot;
    uint8_t report_id;
    uint16_t button_offsets[5]; // buttons 1..5, 0xFFFF if absent
    bool x_relative;            // true for mice
    uint16_t x_offset;
    uint8_t x_size;
    int32_t x_min, x_max;
    uint16_t y_offset;
    uint8_t y_size;
    int32_t y_min, y_max;
    uint16_t wheel_offset; // Wheel/scroll wheel
    uint8_t wheel_size;
    uint16_t pan_offset; // Horizontal pan/tilt
    uint8_t pan_size;
    uint16_t tip_offset;     // Digitizer Tip Switch, 0xFFFF if absent
    uint16_t inrange_offset; // Digitizer In Range, 0xFFFF if absent
} tablet_connection_t;

void tablet_init(void);
void tablet_stop(void);

// Set the extended register value.
bool tablet_xreg(uint16_t word);

// Whether a program has asked for this device's block.
bool tablet_is_mapped(void);

// Parse HID report descriptor for an absolute or relative pointer.
bool tablet_mount(int slot, const tablet_connection_t *desc);

// Clean up descriptor when device is disconnected.
bool tablet_umount(int slot);

// Process HID report.
void tablet_report(int slot, uint8_t const *report, size_t size);

/* A host whose OS decodes its own pointer, in place of a report. It gives
 * canvas pixels, which is what the block carries, so nothing is scaled.
 */

/* A hovering absolute pointer (mouse/pen) at canvas x,y with a button bitmap
 * (TABLET_FLAG_*). Occupies contact 0 and declares a host cursor is available. */
void tablet_host_pointer(int x, int y, uint8_t buttons);

/* n touch contacts (tip down, no hover); the rest go inactive. No host cursor. */
void tablet_host_touch(const tablet_point_t *pts, int n);

/* Accumulate host scroll into the wheel/pan bytes (8-bit wrapping counters),
 * mirroring the mouse. */
void tablet_host_wheel(int dwheel, int dpan);

/* The pointer left the window / all contacts released: everything inactive. */
void tablet_host_clear(void);

/* The ROM's requested cursor shape (control byte, one of TABLET_CURSOR_*); 0 when
 * unmapped. */
uint8_t tablet_control(void);

/* This driver's row in a machine's driver list; see core/sys/driver.h. */
#define TABLET_DRIVER DRIVER(tablet_init, nul_task, nul_task, nul_run, tablet_stop, nul_break, nul_config, nul_config)

#endif /* _CORE_HID_TABLET_H_ */
