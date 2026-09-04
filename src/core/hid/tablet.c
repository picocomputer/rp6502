/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/hid.h"
#include "core/hid/tablet.h"
#include "core/sys/xram.h"
#include "core/vga/vga.h"
#include "machine.h"
#include <string.h>

#if defined(DEBUG_HID) || defined(DEBUG_HID_TABLET)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif


/* XRAM report block, laid out in tablet.h. Every field is one byte, so each 6502 read is atomic; a
 * multi-byte coordinate is delivered as a set of single-byte "windows", exactly
 * one non-zero, decoded first-non-zero-wins. An inactive contact reports flags=0;
 * X/Y are always kept within the canvas. wheel/pan are 8-bit wrapping
 * accumulators read like the mouse's (subtract the previous value). The ROM-owned
 * control byte leads the block so everything the firmware writes back — status,
 * wheel, pan, contacts — is one contiguous run. */
/* A relative mouse reports device counts (mickeys) far finer than a canvas
 * pixel, so it is tracked in a fixed reference resolution at the legacy mouse
 * rate (mouse.c reports counts >>1) and then scaled to the canvas — so the ROM
 * gets absolute XY at a width-independent speed and needs no compensation. */
#define TABLET_REF_WIDTH 640
#define TABLET_REF_HEIGHT 480
#define TABLET_MOUSE_DIV 2 /* counts per reference pixel; matches mouse.c's >>1 */

static uint8_t tablet_state[TABLET_BLOCK_SIZE];
static uint16_t tablet_xram;

/* A machine that lends the ROM its own cursor says so in the status byte,
 * which a fresh mapping has to carry too. A Pico has no such cursor and
 * nothing here ever sets it. */
static bool tablet_host_cursor;

/* Primary pointer, canvas space (what is written to XRAM). */
static int16_t tablet_x;
static int16_t tablet_y;

/* Relative-mouse pointer in the TABLET_REF reference space, with the sub-count
 * remainder carried between reports so slow motion is not lost. */
static int16_t tablet_ref_x;
static int16_t tablet_ref_y;
static int16_t tablet_sub_x;
static int16_t tablet_sub_y;


static tablet_connection_t tablet_connections[TABLET_MAX_MICE];

static tablet_connection_t *tablet_get_connection_by_slot(int slot)
{
    for (int i = 0; i < TABLET_MAX_MICE; ++i)
        if (tablet_connections[i].valid && tablet_connections[i].slot == slot)
            return &tablet_connections[i];
    return NULL;
}

/* X (0..764) -> three single-byte windows, exactly one non-zero. */
static void tablet_encode_x(uint8_t *d, int x)
{
    if (x < 0)
        x = 0;
    if (x > 764)
        x = 764;
    d[0] = d[1] = d[2] = 0;
    if (x <= 254)
        d[0] = (uint8_t)(x + 1);
    else if (x <= 509)
        d[1] = (uint8_t)(x - 254);
    else
        d[2] = (uint8_t)(x - 509);
}

/* Y (0..509) -> two single-byte windows, exactly one non-zero. */
static void tablet_encode_y(uint8_t *d, int y)
{
    if (y < 0)
        y = 0;
    if (y > 509)
        y = 509;
    d[0] = d[1] = 0;
    if (y <= 254)
        d[0] = (uint8_t)(y + 1);
    else
        d[1] = (uint8_t)(y - 254);
}

static void tablet_put_contact(int i, uint8_t flags, int x, int y)
{
    uint8_t *c = &tablet_state[TABLET_OFF_CONTACTS + i * TABLET_CONTACT_SIZE];
    c[0] = flags;
    tablet_encode_x(&c[1], x);
    tablet_encode_y(&c[4], y);
}

static void tablet_clear_contact(int i)
{
    tablet_put_contact(i, 0, 0, 0); /* flags=0 marks it inactive; X/Y stay in-canvas */
}

/* Push everything the firmware owns — status, wheel, pan, contacts — to XRAM in
 * one memcpy; they run contiguously after the ROM-owned control byte at offset 0.
 * Each byte is atomic and the decode tolerates any interleaving, so no barrier
 * is needed. */
static void tablet_write_xram(void)
{
    if (tablet_xram == 0xFFFF)
        return;
    memcpy((uint8_t *)&xram[tablet_xram + TABLET_OFF_STATUS], &tablet_state[TABLET_OFF_STATUS],
            TABLET_BLOCK_SIZE - TABLET_OFF_STATUS);
}

void HOST_IN_FLASH("tablet_init") tablet_init(void)
{
    tablet_stop();
}

void tablet_stop(void)
{
    tablet_xram = 0xFFFF;
}

bool tablet_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - TABLET_BLOCK_SIZE)
        return false;
    tablet_xram = word;
    memset(tablet_state, 0, sizeof(tablet_state));
    if (tablet_host_cursor)
        tablet_state[TABLET_OFF_STATUS] |= TABLET_STATUS_HOST_CURSOR;
    for (int i = 0; i < TABLET_MAX_CONTACTS; ++i)
        tablet_clear_contact(i);
    if (tablet_xram != 0xFFFF) /* one-time full write also seeds control=0 (ROM draws its own) */
        memcpy((uint8_t *)&xram[tablet_xram], tablet_state, TABLET_BLOCK_SIZE);
    return true;
}

bool HOST_IN_FLASH("tablet_mount") tablet_mount(int slot, const tablet_connection_t *desc)
{
    if (!desc->valid)
        return false;
    for (int i = 0; i < TABLET_MAX_MICE; ++i)
    {
        if (tablet_connections[i].valid)
            continue;
        tablet_connections[i] = *desc;
        tablet_connections[i].slot = slot;
        DBG("tablet_mount: slot=%d, x_rel=%d, tip=%d\n", slot, desc->x_relative,
            desc->tip_offset != HID_ABSENT);
        return true;
    }
    return false;
}

bool tablet_umount(int slot)
{
    tablet_connection_t *conn = tablet_get_connection_by_slot(slot);
    if (conn == NULL)
        return false;
    conn->valid = false;
    // Release contact 0 once the last pointer is gone, so a press held at unplug
    // does not stay latched. A surviving device refreshes it on its next report.
    for (int i = 0; i < TABLET_MAX_MICE; ++i)
        if (tablet_connections[i].valid)
            return true;
    tablet_clear_contact(0);
    tablet_write_xram();
    return true;
}

/* Read an axis field, sign-extending when the device declares a signed range
 * (logical_min < 0), matching hid_scale_analog. */
static int32_t tablet_axis_value(const uint8_t *r, uint16_t len, uint16_t off, uint8_t size, int32_t lmin)
{
    if (lmin < 0)
        return hid_extract_signed(r, len, off, size);
    return (int32_t)hid_extract_bits(r, len, off, size);
}

void tablet_report(int slot, uint8_t const *data, size_t size)
{
    tablet_connection_t *conn = tablet_get_connection_by_slot(slot);
    if (conn == NULL)
        return;

    const uint8_t *report_data = data;
    uint16_t report_data_len = size;

    if (conn->report_id != 0)
    {
        if (report_data_len == 0 || report_data[0] != conn->report_id)
            return;
        report_data++;
        report_data_len--;
    }

    int cw, ch;
    vga_canvas_size(&cw, &ch);

    // Position: integrate a relative mouse, scale an absolute digitizer.
    if (conn->x_relative)
    {
        // Accumulate counts at the legacy rate into the reference space (carrying
        // the sub-count remainder), then scale to the canvas.
        tablet_sub_x += (int16_t)hid_extract_signed(report_data, report_data_len, conn->x_offset, conn->x_size);
        tablet_sub_y += (int16_t)hid_extract_signed(report_data, report_data_len, conn->y_offset, conn->y_size);
        int sx = tablet_sub_x / TABLET_MOUSE_DIV;
        int sy = tablet_sub_y / TABLET_MOUSE_DIV;
        tablet_sub_x -= (int16_t)(sx * TABLET_MOUSE_DIV);
        tablet_sub_y -= (int16_t)(sy * TABLET_MOUSE_DIV);
        tablet_ref_x += (int16_t)sx;
        tablet_ref_y += (int16_t)sy;
        if (tablet_ref_x < 0)
            tablet_ref_x = 0;
        else if (tablet_ref_x > TABLET_REF_WIDTH - 1)
            tablet_ref_x = TABLET_REF_WIDTH - 1;
        if (tablet_ref_y < 0)
            tablet_ref_y = 0;
        else if (tablet_ref_y > TABLET_REF_HEIGHT - 1)
            tablet_ref_y = TABLET_REF_HEIGHT - 1;
        // One isotropic gain (the width ratio) on both axes keeps motion
        // pixel-square; the clamp below bounds the vertical extent.
        tablet_x = (int16_t)((int32_t)tablet_ref_x * cw / TABLET_REF_WIDTH);
        tablet_y = (int16_t)((int32_t)tablet_ref_y * cw / TABLET_REF_WIDTH);
    }
    else
    {
        int32_t rx = tablet_axis_value(report_data, report_data_len, conn->x_offset, conn->x_size, conn->x_min);
        int32_t ry = tablet_axis_value(report_data, report_data_len, conn->y_offset, conn->y_size, conn->y_min);
        int32_t xs = conn->x_max - conn->x_min;
        int32_t ys = conn->y_max - conn->y_min;
        if (xs > 0)
            tablet_x = (int16_t)(((int64_t)(rx - conn->x_min) * (cw - 1)) / xs);
        if (ys > 0)
            tablet_y = (int16_t)(((int64_t)(ry - conn->y_min) * (ch - 1)) / ys);
    }
    if (tablet_x < 0)
        tablet_x = 0;
    else if (tablet_x > cw - 1)
        tablet_x = cw - 1;
    if (tablet_y < 0)
        tablet_y = 0;
    else if (tablet_y > ch - 1)
        tablet_y = ch - 1;

    // An absolute device set tablet_x/tablet_y directly; keep the relative-mouse
    // reference in step so a later mouse continues from here instead of snapping
    // back to a stale position.
    if (!conn->x_relative)
    {
        tablet_ref_x = (int16_t)((int32_t)tablet_x * TABLET_REF_WIDTH / cw);
        tablet_ref_y = (int16_t)((int32_t)tablet_y * TABLET_REF_WIDTH / cw);
        tablet_sub_x = tablet_sub_y = 0;
    }

    // Buttons: mouse buttons 1..5, plus a digitizer Tip Switch as the primary.
    uint8_t buttons = 0;
    for (int i = 0; i < 5; i++)
        if (conn->button_offsets[i] != 0xFFFF)
            if (hid_extract_bits(report_data, report_data_len, conn->button_offsets[i], 1))
                buttons |= (uint8_t)(1 << i);
    if (conn->tip_offset != 0xFFFF)
        if (hid_extract_bits(report_data, report_data_len, conn->tip_offset, 1))
            buttons |= TABLET_FLAG_LEFT;

    // Hover: a mouse always tracks; an absolute pen tracks when In Range; a bare
    // touchscreen (tip only, no In Range) does not.
    bool hover = conn->x_relative;
    if (!conn->x_relative && conn->inrange_offset != 0xFFFF)
        hover = hid_extract_bits(report_data, report_data_len, conn->inrange_offset, 1) != 0;

    // Scroll: only a mouse carries these; a pen/touch has wheel_size 0 and is skipped.
    if (conn->wheel_size > 0)
        tablet_state[TABLET_OFF_WHEEL] += hid_extract_signed(report_data, report_data_len,
                                                             conn->wheel_offset, conn->wheel_size);
    if (conn->pan_size > 0)
        tablet_state[TABLET_OFF_PAN] += hid_extract_signed(report_data, report_data_len,
                                                           conn->pan_offset, conn->pan_size);

    tablet_put_contact(0, (uint8_t)(buttons | (hover ? TABLET_FLAG_HOVER : 0)), tablet_x, tablet_y);
    tablet_write_xram();
}

bool tablet_is_mapped(void)
{
    return tablet_xram != 0xFFFF;
}

static void tablet_set_host_cursor(bool on)
{
    tablet_host_cursor = on;
    if (on)
        tablet_state[TABLET_OFF_STATUS] |= TABLET_STATUS_HOST_CURSOR;
    else
        tablet_state[TABLET_OFF_STATUS] &= (uint8_t)~TABLET_STATUS_HOST_CURSOR;
}

void tablet_host_pointer(int x, int y, uint8_t buttons)
{
    tablet_set_host_cursor(true);
    tablet_put_contact(0, (uint8_t)(buttons | TABLET_FLAG_HOVER), x, y);
    for (int i = 1; i < TABLET_MAX_CONTACTS; ++i)
        tablet_clear_contact(i);
    tablet_write_xram();
}

void tablet_host_touch(const tablet_point_t *pts, int n)
{
    tablet_set_host_cursor(false); // a finger has no cursor
    if (n > TABLET_MAX_CONTACTS)
        n = TABLET_MAX_CONTACTS;
    for (int i = 0; i < n; ++i)
        tablet_put_contact(i, TABLET_FLAG_LEFT, pts[i].x, pts[i].y); // tip down, no hover
    for (int i = n; i < TABLET_MAX_CONTACTS; ++i)
        tablet_clear_contact(i);
    tablet_write_xram();
}

void tablet_host_clear(void)
{
    for (int i = 0; i < TABLET_MAX_CONTACTS; ++i)
        tablet_clear_contact(i);
    tablet_write_xram();
}

void tablet_host_wheel(int dwheel, int dpan)
{
    if (dwheel == 0 && dpan == 0)
        return;
    tablet_state[TABLET_OFF_WHEEL] = (uint8_t)(tablet_state[TABLET_OFF_WHEEL] + dwheel);
    tablet_state[TABLET_OFF_PAN] = (uint8_t)(tablet_state[TABLET_OFF_PAN] + dpan);
    tablet_write_xram();
}

uint8_t tablet_control(void)
{
    if (tablet_xram == 0xFFFF)
        return TABLET_CURSOR_OFF;
    return xram[tablet_xram + TABLET_OFF_CONTROL];
}
