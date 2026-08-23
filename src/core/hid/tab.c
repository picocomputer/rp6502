/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/hid.h"
#include "core/hid/tab.h"
#include "core/mem.h"
#include "core/vga/vga.h"
#include <pico.h>
#include <string.h>

#if defined(DEBUG_RIA_HID) || defined(DEBUG_RIA_HID_TAB)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif


/* XRAM report block, laid out in tab.h. Every field is one byte, so each 6502 read is atomic; a
 * multi-byte coordinate is delivered as a set of single-byte "windows", exactly
 * one non-zero, decoded first-non-zero-wins. An inactive contact reports flags=0;
 * X/Y are always kept within the canvas. wheel/pan are 8-bit wrapping
 * accumulators read like the mouse's (subtract the previous value). The ROM-owned
 * control byte leads the block so everything the firmware writes back — status,
 * wheel, pan, contacts — is one contiguous run. */
/* A relative mouse reports device counts (mickeys) far finer than a canvas
 * pixel, so it is tracked in a fixed reference resolution at the legacy mouse
 * rate (mou.c reports counts >>1) and then scaled to the canvas — so the ROM
 * gets absolute XY at a width-independent speed and needs no compensation. */
#define TAB_REF_WIDTH 640
#define TAB_REF_HEIGHT 480
#define TAB_MOUSE_DIV 2 /* counts per reference pixel; matches mou.c's >>1 */

static uint8_t tab_state[TAB_BLOCK_SIZE];
static uint16_t tab_xram;

/* A machine that lends the ROM its own cursor says so in the status byte,
 * which a fresh mapping has to carry too. A Pico has no such cursor and
 * nothing here ever sets it. */
static bool tab_host_cursor;

/* Primary pointer, canvas space (what is written to XRAM). */
static int16_t tab_x;
static int16_t tab_y;

/* Relative-mouse pointer in the TAB_REF reference space, with the sub-count
 * remainder carried between reports so slow motion is not lost. */
static int16_t tab_ref_x;
static int16_t tab_ref_y;
static int16_t tab_sub_x;
static int16_t tab_sub_y;


static tab_connection_t tab_connections[TAB_MAX_MICE];

static tab_connection_t *tab_get_connection_by_slot(int slot)
{
    for (int i = 0; i < TAB_MAX_MICE; ++i)
        if (tab_connections[i].valid && tab_connections[i].slot == slot)
            return &tab_connections[i];
    return NULL;
}

/* X (0..764) -> three single-byte windows, exactly one non-zero. */
static void tab_encode_x(uint8_t *d, int x)
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
static void tab_encode_y(uint8_t *d, int y)
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

static void tab_put_contact(int i, uint8_t flags, int x, int y)
{
    uint8_t *c = &tab_state[TAB_OFF_CONTACTS + i * TAB_CONTACT_SIZE];
    c[0] = flags;
    tab_encode_x(&c[1], x);
    tab_encode_y(&c[4], y);
}

static void tab_clear_contact(int i)
{
    tab_put_contact(i, 0, 0, 0); /* flags=0 marks it inactive; X/Y stay in-canvas */
}

/* Push everything the firmware owns — status, wheel, pan, contacts — to XRAM in
 * one memcpy; they run contiguously after the ROM-owned control byte at offset 0.
 * Each byte is atomic and the decode tolerates any interleaving, so no barrier
 * is needed. */
static void tab_write_xram(void)
{
    if (tab_xram == 0xFFFF)
        return;
    memcpy((uint8_t *)&xram[tab_xram + TAB_OFF_STATUS], &tab_state[TAB_OFF_STATUS],
           TAB_BLOCK_SIZE - TAB_OFF_STATUS);
}

void __in_flash("tab_init") tab_init(void)
{
    tab_stop();
}

void tab_stop(void)
{
    tab_xram = 0xFFFF;
}

bool tab_xreg(uint16_t word)
{
    if (word != 0xFFFF && word > 0x10000 - TAB_BLOCK_SIZE)
        return false;
    tab_xram = word;
    memset(tab_state, 0, sizeof(tab_state));
    if (tab_host_cursor)
        tab_state[TAB_OFF_STATUS] |= TAB_STATUS_HOST_CURSOR;
    for (int i = 0; i < TAB_MAX_CONTACTS; ++i)
        tab_clear_contact(i);
    if (tab_xram != 0xFFFF) /* one-time full write also seeds control=0 (ROM draws its own) */
        memcpy((uint8_t *)&xram[tab_xram], tab_state, TAB_BLOCK_SIZE);
    return true;
}

bool __in_flash("tab_mount") tab_mount(int slot, const tab_connection_t *desc)
{
    if (!desc->valid)
        return false;
    for (int i = 0; i < TAB_MAX_MICE; ++i)
    {
        if (tab_connections[i].valid)
            continue;
        tab_connections[i] = *desc;
        tab_connections[i].slot = slot;
        DBG("tab_mount: slot=%d, x_rel=%d, tip=%d\n", slot, desc->x_relative,
            desc->tip_offset != HID_ABSENT);
        return true;
    }
    return false;
}

bool tab_umount(int slot)
{
    tab_connection_t *conn = tab_get_connection_by_slot(slot);
    if (conn == NULL)
        return false;
    conn->valid = false;
    // Release contact 0 once the last pointer is gone, so a press held at unplug
    // does not stay latched. A surviving device refreshes it on its next report.
    for (int i = 0; i < TAB_MAX_MICE; ++i)
        if (tab_connections[i].valid)
            return true;
    tab_clear_contact(0);
    tab_write_xram();
    return true;
}

/* Read an axis field, sign-extending when the device declares a signed range
 * (logical_min < 0), matching hid_scale_analog. */
static int32_t tab_axis_value(const uint8_t *r, uint16_t len, uint16_t off, uint8_t size, int32_t lmin)
{
    if (lmin < 0)
        return hid_extract_signed(r, len, off, size);
    return (int32_t)hid_extract_bits(r, len, off, size);
}

void tab_report(int slot, uint8_t const *data, size_t size)
{
    tab_connection_t *conn = tab_get_connection_by_slot(slot);
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
        tab_sub_x += (int16_t)hid_extract_signed(report_data, report_data_len, conn->x_offset, conn->x_size);
        tab_sub_y += (int16_t)hid_extract_signed(report_data, report_data_len, conn->y_offset, conn->y_size);
        int sx = tab_sub_x / TAB_MOUSE_DIV;
        int sy = tab_sub_y / TAB_MOUSE_DIV;
        tab_sub_x -= (int16_t)(sx * TAB_MOUSE_DIV);
        tab_sub_y -= (int16_t)(sy * TAB_MOUSE_DIV);
        tab_ref_x += (int16_t)sx;
        tab_ref_y += (int16_t)sy;
        if (tab_ref_x < 0)
            tab_ref_x = 0;
        else if (tab_ref_x > TAB_REF_WIDTH - 1)
            tab_ref_x = TAB_REF_WIDTH - 1;
        if (tab_ref_y < 0)
            tab_ref_y = 0;
        else if (tab_ref_y > TAB_REF_HEIGHT - 1)
            tab_ref_y = TAB_REF_HEIGHT - 1;
        // One isotropic gain (the width ratio) on both axes keeps motion
        // pixel-square; the clamp below bounds the vertical extent.
        tab_x = (int16_t)((int32_t)tab_ref_x * cw / TAB_REF_WIDTH);
        tab_y = (int16_t)((int32_t)tab_ref_y * cw / TAB_REF_WIDTH);
    }
    else
    {
        int32_t rx = tab_axis_value(report_data, report_data_len, conn->x_offset, conn->x_size, conn->x_min);
        int32_t ry = tab_axis_value(report_data, report_data_len, conn->y_offset, conn->y_size, conn->y_min);
        int32_t xs = conn->x_max - conn->x_min;
        int32_t ys = conn->y_max - conn->y_min;
        if (xs > 0)
            tab_x = (int16_t)(((int64_t)(rx - conn->x_min) * (cw - 1)) / xs);
        if (ys > 0)
            tab_y = (int16_t)(((int64_t)(ry - conn->y_min) * (ch - 1)) / ys);
    }
    if (tab_x < 0)
        tab_x = 0;
    else if (tab_x > cw - 1)
        tab_x = cw - 1;
    if (tab_y < 0)
        tab_y = 0;
    else if (tab_y > ch - 1)
        tab_y = ch - 1;

    // An absolute device set tab_x/tab_y directly; keep the relative-mouse
    // reference in step so a later mouse continues from here instead of snapping
    // back to a stale position.
    if (!conn->x_relative)
    {
        tab_ref_x = (int16_t)((int32_t)tab_x * TAB_REF_WIDTH / cw);
        tab_ref_y = (int16_t)((int32_t)tab_y * TAB_REF_WIDTH / cw);
        tab_sub_x = tab_sub_y = 0;
    }

    // Buttons: mouse buttons 1..5, plus a digitizer Tip Switch as the primary.
    uint8_t buttons = 0;
    for (int i = 0; i < 5; i++)
        if (conn->button_offsets[i] != 0xFFFF)
            if (hid_extract_bits(report_data, report_data_len, conn->button_offsets[i], 1))
                buttons |= (uint8_t)(1 << i);
    if (conn->tip_offset != 0xFFFF)
        if (hid_extract_bits(report_data, report_data_len, conn->tip_offset, 1))
            buttons |= TAB_FLAG_LEFT;

    // Hover: a mouse always tracks; an absolute pen tracks when In Range; a bare
    // touchscreen (tip only, no In Range) does not.
    bool hover = conn->x_relative;
    if (!conn->x_relative && conn->inrange_offset != 0xFFFF)
        hover = hid_extract_bits(report_data, report_data_len, conn->inrange_offset, 1) != 0;

    // Scroll: only a mouse carries these; a pen/touch has wheel_size 0 and is skipped.
    if (conn->wheel_size > 0)
        tab_state[TAB_OFF_WHEEL] += hid_extract_signed(report_data, report_data_len,
                                                       conn->wheel_offset, conn->wheel_size);
    if (conn->pan_size > 0)
        tab_state[TAB_OFF_PAN] += hid_extract_signed(report_data, report_data_len,
                                                     conn->pan_offset, conn->pan_size);

    tab_put_contact(0, (uint8_t)(buttons | (hover ? TAB_FLAG_HOVER : 0)), tab_x, tab_y);
    tab_write_xram();
}

bool tab_is_mapped(void)
{
    return tab_xram != 0xFFFF;
}

static void tab_set_host_cursor(bool on)
{
    tab_host_cursor = on;
    if (on)
        tab_state[TAB_OFF_STATUS] |= TAB_STATUS_HOST_CURSOR;
    else
        tab_state[TAB_OFF_STATUS] &= (uint8_t)~TAB_STATUS_HOST_CURSOR;
}

void tab_host_pointer(int x, int y, uint8_t buttons)
{
    tab_set_host_cursor(true);
    tab_put_contact(0, (uint8_t)(buttons | TAB_FLAG_HOVER), x, y);
    for (int i = 1; i < TAB_MAX_CONTACTS; ++i)
        tab_clear_contact(i);
    tab_write_xram();
}

void tab_host_touch(const tab_point_t *pts, int n)
{
    tab_set_host_cursor(false); // a finger has no cursor
    if (n > TAB_MAX_CONTACTS)
        n = TAB_MAX_CONTACTS;
    for (int i = 0; i < n; ++i)
        tab_put_contact(i, TAB_FLAG_LEFT, pts[i].x, pts[i].y); // tip down, no hover
    for (int i = n; i < TAB_MAX_CONTACTS; ++i)
        tab_clear_contact(i);
    tab_write_xram();
}

void tab_host_clear(void)
{
    for (int i = 0; i < TAB_MAX_CONTACTS; ++i)
        tab_clear_contact(i);
    tab_write_xram();
}

void tab_host_wheel(int dwheel, int dpan)
{
    if (dwheel == 0 && dpan == 0)
        return;
    tab_state[TAB_OFF_WHEEL] = (uint8_t)(tab_state[TAB_OFF_WHEEL] + dwheel);
    tab_state[TAB_OFF_PAN] = (uint8_t)(tab_state[TAB_OFF_PAN] + dpan);
    tab_write_xram();
}

uint8_t tab_control(void)
{
    if (tab_xram == 0xFFFF)
        return TAB_CURSOR_OFF;
    return xram[tab_xram + TAB_OFF_CONTROL];
}
