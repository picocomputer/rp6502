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
#define TABLET_CONTACT_SIZE 4 /* flags, y_lo, x_lo, yx_hi */
#define TABLET_BLOCK_SIZE (TABLET_HEADER_SIZE + TABLET_MAX_CONTACTS * TABLET_CONTACT_SIZE)

#define TABLET_OFF_CONTROL 0
#define TABLET_OFF_STATUS 1
#define TABLET_OFF_WHEEL 2
#define TABLET_OFF_PAN 3
#define TABLET_OFF_CONTACTS 4

/* status (the program's): mapping sets it to TABLET_STATUS_SHARED, and the
 * firmware never writes it again. While its bit 0 is set the program shares
 * the tablet with the console's terminal (core/term/vterm_mouse.h); a
 * program that clears it has the tablet to itself. */
#define TABLET_STATUS_SHARED 0x01

/* contact flags. Hover is bit 7 so that the 6502 can test it with BIT and
 * BMI. */
#define TABLET_FLAG_LEFT 0x01
#define TABLET_FLAG_RIGHT 0x02
#define TABLET_FLAG_MIDDLE 0x04
#define TABLET_FLAG_BTN4 0x08
#define TABLET_FLAG_BTN5 0x10
#define TABLET_FLAG_HOVER 0x80

/* control (program to firmware): the cursor shape the program asks for. The
 * host draws it where it has a cursor of its own, and the system draws it
 * over the canvas everywhere else (core/vga/pointer.h). */
enum
{
    TABLET_CURSOR_OFF = 0, /* no cursor: the program draws its own, or none */
    TABLET_CURSOR_ARROW,
    TABLET_CURSOR_CROSSHAIR,
    TABLET_CURSOR_IBEAM,
    TABLET_CURSOR_HAND,
    TABLET_CURSOR_RESIZE_EW,
    TABLET_CURSOR_RESIZE_NS,
    TABLET_CURSOR_RESIZE_NWSE,
    TABLET_CURSOR_RESIZE_NESW,
    TABLET_CURSOR_MOVE,
    TABLET_CURSOR_NOT_ALLOWED,
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
    bool x_relative;            // true for a relative mouse
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
    /* An absolute device's physical width and height in the descriptor's
     * units, or 0 where it declares none, for its shape. */
    int32_t x_phys, y_phys;
    bool touchscreen; // over the display, as an absolute mouse is too
    uint8_t flags;    // this pointer's TABLET_FLAG_ buttons and hover
} tablet_connection_t;

void tablet_init(void);
/* Unmaps the block. It is also the RESET hook: a failed load can leave the
 * address pointing into the next program's XRAM, and the next mapping
 * rewrites the block. */
void tablet_stop(void);

bool tablet_xreg(uint16_t word);

bool tablet_is_mapped(void);

/* Whether the program shares the tablet (TABLET_STATUS_SHARED), read from
 * its block as it is now. An unmapped tablet is no program's, so it is
 * shared. It runs from RAM, so the owner can read it as a change comes. */
bool tablet_shared(void);

/* Whether the tablet wants the host's pointer: the host delivers pointer
 * events, shows the shape from tablet_shape, and keeps the pointer out of
 * the browser's hands. A program has mapped it, or its owner claims it. A
 * host that captures its pointer for the mouse still does so for a program
 * that maps the mouse and not the tablet, which is not on the tablet. */
bool tablet_wanted(void);

/* Whether the tablet wants the host's wheel and sideways scrolling: a
 * program has mapped it, so its counters are read, or its owner claims the
 * wheel. A host with a page around it leaves an unwanted wheel to scroll
 * the page. */
bool tablet_wheel_wanted(void);

bool tablet_mount(int slot, const tablet_connection_t *desc);

bool tablet_umount(int slot);

void tablet_report(int slot, uint8_t const *report, size_t size);

/* A hovering pointer at x,y in canvas pixels, which is what the block carries,
 * with a bitmap of TABLET_FLAG_ buttons, taking contact 0. host_cursor says
 * whether this host draws the cursor itself; where it does not, the system
 * draws it over the canvas. */
void tablet_host_pointer(int x, int y, uint8_t buttons, bool host_cursor);

/* n touch contacts in canvas pixels, tip down and not hovering. The rest go
 * inactive. */
void tablet_host_touch(const tablet_point_t *pts, int n);

/* The wheel and pan bytes are counters a program reads by subtracting the
 * value it saw last, so these add to them and wrap, as the mouse's do. */
void tablet_host_wheel(int dwheel, int dpan);

/* The pointer left the window, or every contact was released. */
void tablet_host_clear(void);

/* A host says on every poll whether it gives its pointer to the tablet. As
 * it stops, the pointer is let go (tablet_host_clear), so no button is left
 * held on the tablet; while it does not give it, nothing is cleared, so the
 * tablet keeps whatever else puts a contact there. The last answer is saved
 * with the machine, so the edge holds across a state load, runahead's
 * included. */
void tablet_host_give(bool given);

/* The cursor shape over the canvas, one of TABLET_CURSOR_: the one the
 * program asked for, or TABLET_CURSOR_OFF for one it does not know. With no
 * program on the tablet, the arrow while its owner claims it, and otherwise
 * none. */
uint8_t tablet_shape(void);

/* ---- the tablet's owner -------------------------------------------------
 *
 * The machine's console (core/term/vterm_mouse.c) supplies these, on every
 * machine that has the tablet. */

/* Contact 0 or the wheel and pan counters changed: every change, mapped or
 * not, once. old_flags and flags are contact 0's TABLET_FLAG_ before and
 * after, x and y where it is now in canvas pixels, and dwheel and dpan what
 * the counters moved by. moved_by_user is false for a pointer the firmware
 * moved, as onto a new canvas, or one that did not move. It runs from RAM,
 * as the HID reports do. */
void tablet_on_contact(uint8_t old_flags, uint8_t flags, int16_t x, int16_t y,
                       int8_t dwheel, int8_t dpan, bool moved_by_user);

/* Whether the owner wants the pointer with no program on the tablet. */
bool tablet_claimed(void);

/* Whether the owner does something with the wheel and sideways scrolling,
 * mapped or not. */
bool tablet_wheel_claimed(void);

extern const sst_table_t tablet_sst;

/* Puts the system's pointer where the tablet is, in the shape tablet_shape
 * gives, while it hovers and the host draws no cursor of its own. */
void tablet_task(void);

#define TABLET_DRIVER DRIVER(                                        \
    INIT(tablet_init)                                                \
    TASK_PUMPED(tablet_task)                                         \
    STOP(tablet_stop)                                                \
    RESET(tablet_stop)                                               \
    SST("TBLT", tablet))

#endif /* _CORE_HID_TABLET_H_ */
