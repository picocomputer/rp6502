/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_HID_TABLET_H_
#define _CORE_HID_TABLET_H_

/* The absolute pointer, called a tablet here. It reports a position on the
 * canvas rather than relative motion: a relative mouse is integrated and
 * clamped to the canvas, and an absolute digitizer or pen is scaled to it. The
 * XRAM block is laid out in tablet.c.
 */

#include <stddef.h>
#include <stdint.h>
#include "core/sys/sst.h"
#include <stdbool.h>

#include "core/hid/hid.h"

#define TABLET_MAX_CONTACTS 8 /* fixed, because the program allocates the block */
#define TABLET_HEADER_SIZE 4  /* control, status, wheel, pan */
#define TABLET_CONTACT_SIZE 6 /* flags, x0, x1, x2, y0, y1 */
#define TABLET_BLOCK_SIZE (TABLET_HEADER_SIZE + TABLET_MAX_CONTACTS * TABLET_CONTACT_SIZE)

#define TABLET_OFF_CONTROL 0
#define TABLET_OFF_STATUS 1
#define TABLET_OFF_WHEEL 2
#define TABLET_OFF_PAN 3
#define TABLET_OFF_CONTACTS 4

/* status (firmware to ROM). A machine that has a cursor of its own to lend the
 * program says so here; a Pico has none and never sets it. */
#define TABLET_STATUS_HOST_CURSOR 0x01

/* contact flags. Hover is bit 7 so that the 6502 can test it with BIT and
 * BMI. */
#define TABLET_FLAG_LEFT 0x01
#define TABLET_FLAG_RIGHT 0x02
#define TABLET_FLAG_MIDDLE 0x04
#define TABLET_FLAG_BTN4 0x08
#define TABLET_FLAG_BTN5 0x10
#define TABLET_FLAG_HOVER 0x80

/* control (program to firmware): the cursor shapes a program may ask for. A
 * host with no cursor of its own ignores them. */
enum
{
    TABLET_CURSOR_OFF = 0, /* the host cursor is hidden and the program draws its own */
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

#define TABLET_MAX_MICE 4

typedef struct tablet_connection
{
    bool valid;
    int slot;
    uint8_t report_id;
    uint16_t button_offsets[5]; // buttons 1 to 5, HID_ABSENT if absent
    bool x_relative;            // true for a mouse
    uint16_t x_offset;
    uint8_t x_size;
    int32_t x_min, x_max;
    uint16_t y_offset;
    uint8_t y_size;
    int32_t y_min, y_max;
    uint16_t wheel_offset;
    uint8_t wheel_size;
    uint16_t pan_offset; // horizontal scroll
    uint8_t pan_size;
    uint16_t tip_offset;     // Digitizer Tip Switch, HID_ABSENT if absent
    uint16_t inrange_offset; // Digitizer In Range, HID_ABSENT if absent
} tablet_connection_t;

void tablet_init(void);
void tablet_stop(void);

bool tablet_xreg(uint16_t word);

bool tablet_is_mapped(void);

bool tablet_mount(int slot, const tablet_connection_t *desc);

bool tablet_umount(int slot);

void tablet_report(int slot, uint8_t const *report, size_t size);

/* A hovering pointer at x,y in canvas pixels, which is what the block carries,
 * with a bitmap of TABLET_FLAG_ buttons, taking contact 0. host_cursor says
 * whether this host can draw a cursor for the program; a host that cannot says
 * so and the program draws its own. */
void tablet_host_pointer(int x, int y, uint8_t buttons, bool host_cursor);

/* n touch contacts in canvas pixels, tip down and not hovering. The rest go
 * inactive. */
void tablet_host_touch(const tablet_point_t *pts, int n);

/* The wheel and pan bytes are counters a program reads by subtracting the
 * value it saw last, so these add to them and wrap, as the mouse's do. */
void tablet_host_wheel(int dwheel, int dpan);

/* The pointer left the window, or every contact was released. */
void tablet_host_clear(void);

/* The cursor shape the program asked for, one of TABLET_CURSOR_. */
uint8_t tablet_control(void);

/* The blob carries the block and whether a host lends its own cursor:
 * 2 + TABLET_BLOCK_SIZE + 1. The six counters behind the block are not saved,
 * because only tablet_report writes them and no machine that saves state has
 * a report to give it. */
#define TABLET_SST_SIZE (3 + TABLET_BLOCK_SIZE)
void tablet_sst_save(sst_cursor_t *c, unsigned flags);
bool tablet_sst_load(sst_cursor_t *c, unsigned flags);

#define TABLET_DRIVER DRIVER(tablet_init, nul_task, nul_task, nul_run, tablet_stop, nul_break, \
    nul_config, nul_config, SST(TBLT, 1, TABLET_SST_SIZE, tablet_sst_save, tablet_sst_load))

#endif /* _CORE_HID_TABLET_H_ */
