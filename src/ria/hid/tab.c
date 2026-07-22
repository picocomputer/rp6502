/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ria/hid/hid.h"
#include "ria/hid/tab.h"
#include "ria/sys/mem.h"
#include "ria/sys/vga.h"
#include <pico.h>
#include <string.h>

#if defined(DEBUG_RIA_HID) || defined(DEBUG_RIA_HID_TAB)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

#define TAB_MAX_MICE 4

/* XRAM report block. Every field is one byte, so each 6502 read is atomic; a
 * multi-byte coordinate is delivered as a set of single-byte "windows", exactly
 * one non-zero, decoded first-non-zero-wins. An inactive contact reports flags=0;
 * X/Y are always kept within the canvas. wheel/pan are 8-bit wrapping
 * accumulators read like the mouse's (subtract the previous value). The ROM-owned
 * control byte leads the block so everything the firmware writes back — status,
 * wheel, pan, contacts — is one contiguous run. */
#define TAB_MAX_CONTACTS 8
#define TAB_HEADER_SIZE 4  /* control, status, wheel, pan */
#define TAB_CONTACT_SIZE 6 /* flags, x0, x1, x2, y0, y1 */
#define TAB_BLOCK_SIZE (TAB_HEADER_SIZE + TAB_MAX_CONTACTS * TAB_CONTACT_SIZE)

#define TAB_OFF_CONTROL 0
#define TAB_OFF_STATUS 1
#define TAB_OFF_WHEEL 2
#define TAB_OFF_PAN 3
#define TAB_OFF_CONTACTS 4

/* status (fw->ROM): host_cursor is always 0 on real hardware (no host cursor). */
#define TAB_STATUS_HOST_CURSOR 0x01

/* contact flags (b7 hover so the 6502 tests it with BIT/BMI). */
#define TAB_FLAG_LEFT 0x01
#define TAB_FLAG_RIGHT 0x02
#define TAB_FLAG_MIDDLE 0x04
#define TAB_FLAG_BTN4 0x08
#define TAB_FLAG_BTN5 0x10
#define TAB_FLAG_HOVER 0x80

/* A relative mouse reports device counts (mickeys) far finer than a canvas
 * pixel, so it is tracked in a fixed reference resolution at the legacy mouse
 * rate (mou.c reports counts >>1) and then scaled to the canvas — so the ROM
 * gets absolute XY at a width-independent speed and needs no compensation. */
#define TAB_REF_WIDTH 640
#define TAB_REF_HEIGHT 480
#define TAB_MOUSE_DIV 2 /* counts per reference pixel; matches mou.c's >>1 */

static uint8_t tab_state[TAB_BLOCK_SIZE];
static uint16_t tab_xram;

/* Primary pointer, canvas space (what is written to XRAM). */
static int16_t tab_x;
static int16_t tab_y;

/* Relative-mouse pointer in the TAB_REF reference space, with the sub-count
 * remainder carried between reports so slow motion is not lost. */
static int16_t tab_ref_x;
static int16_t tab_ref_y;
static int16_t tab_sub_x;
static int16_t tab_sub_y;

typedef struct
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
} tab_connection_t;

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
    memcpy(&xram[tab_xram + TAB_OFF_STATUS], &tab_state[TAB_OFF_STATUS],
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
    memset(tab_state, 0, sizeof(tab_state)); /* status host_cursor=0: no host cursor on hardware */
    for (int i = 0; i < TAB_MAX_CONTACTS; ++i)
        tab_clear_contact(i);
    if (tab_xram != 0xFFFF) /* one-time full write also seeds control=0 (ROM draws its own) */
        memcpy(&xram[tab_xram], tab_state, TAB_BLOCK_SIZE);
    return true;
}

/* Pass 1: pick which report a multi-collection device drives us with. A digitizer
 * (Tip Switch) wins over a bare Generic-Desktop X/Y, so a pen/touch panel that
 * also exposes a mouse-compatibility collection is decoded as the absolute
 * digitizer rather than as its relative-mouse alias. */
typedef struct
{
    uint16_t rid_digitizer; // report id carrying a Tip Switch, 0xFFFF if none
    uint16_t rid_xy;        // first report id carrying GD X/Y, 0xFFFF if none
} tab_select_t;

static bool __in_flash("tab_select") tab_select_report(const hid_field_t *field, void *context)
{
    tab_select_t *sel = (tab_select_t *)context;
    if (field->usage_page == 0x0D && field->usage == 0x42) // Digitizer Tip Switch
    {
        if (sel->rid_digitizer == 0xFFFF)
            sel->rid_digitizer = field->report_id;
    }
    else if (field->usage_page == 0x01 &&
             (field->usage == 0x30 || field->usage == 0x31)) // GD X or Y
    {
        if (sel->rid_xy == 0xFFFF)
            sel->rid_xy = field->report_id;
    }
    return true;
}

/* Pass 2: extract the chosen report's fields. Offsets are first-wins, so a
 * multi-touch descriptor (several Finger collections sharing one report id) binds
 * to the first contact's fields — the slot a single-finger report fills. */
static bool __in_flash("tab_parse") tab_parse_field(const hid_field_t *field, void *context)
{
    tab_connection_t *conn = (tab_connection_t *)context;

    if (conn->report_id != 0 && field->report_id != 0xFFFF &&
        field->report_id != conn->report_id)
        return true;

    if (field->usage_page == 0x01) // Generic Desktop
    {
        switch (field->usage)
        {
        case 0x30: // X
            if (conn->x_size == 0)
            {
                conn->x_offset = field->bit_pos;
                conn->x_size = field->size;
                conn->x_relative = (field->input_flags & 0x04) != 0;
                conn->x_min = field->logical_min;
                conn->x_max = field->logical_max;
            }
            break;
        case 0x31: // Y
            if (conn->y_size == 0)
            {
                conn->y_offset = field->bit_pos;
                conn->y_size = field->size;
                conn->y_min = field->logical_min;
                conn->y_max = field->logical_max;
            }
            break;
        case 0x38: // Wheel
            if (conn->wheel_size == 0)
            {
                conn->wheel_offset = field->bit_pos;
                conn->wheel_size = field->size;
            }
            break;
        case 0x3C: // Pan/horizontal wheel
            if (conn->pan_size == 0)
            {
                conn->pan_offset = field->bit_pos;
                conn->pan_size = field->size;
            }
            break;
        }
    }
    else if (field->usage_page == 0x0C) // Consumer
    {
        if (field->usage == 0x238 && conn->pan_size == 0) // AC Pan
        {
            conn->pan_offset = field->bit_pos;
            conn->pan_size = field->size;
        }
    }
    else if (field->usage_page == 0x09) // Button
    {
        if (field->usage >= 1 && field->usage <= 5 &&
            conn->button_offsets[field->usage - 1] == 0xFFFF)
            conn->button_offsets[field->usage - 1] = field->bit_pos;
    }
    else if (field->usage_page == 0x0D) // Digitizer
    {
        if (field->usage == 0x42 && conn->tip_offset == 0xFFFF) // Tip Switch
            conn->tip_offset = field->bit_pos;
        else if (field->usage == 0x32 && conn->inrange_offset == 0xFFFF) // In Range
            conn->inrange_offset = field->bit_pos;
    }

    return true;
}

bool __in_flash("tab_mount") tab_mount(int slot, uint8_t const *desc_data, uint16_t desc_len)
{
    int conn_num = -1;
    for (int i = 0; i < TAB_MAX_MICE; ++i)
        if (!tab_connections[i].valid)
        {
            conn_num = i;
            break;
        }
    if (conn_num < 0)
        return false;

    tab_connection_t *conn = &tab_connections[conn_num];
    memset(conn, 0, sizeof(tab_connection_t));
    for (int i = 0; i < 5; i++)
        conn->button_offsets[i] = 0xFFFF;
    conn->tip_offset = 0xFFFF;
    conn->inrange_offset = 0xFFFF;
    conn->slot = slot;

    // Pass 1: choose the report id, preferring a digitizer over a mouse-compat one.
    tab_select_t sel = {0xFFFF, 0xFFFF};
    hid_descriptor_parse(desc_data, desc_len, tab_select_report, &sel);
    uint16_t chosen = sel.rid_digitizer != 0xFFFF ? sel.rid_digitizer : sel.rid_xy;
    conn->report_id = chosen == 0xFFFF ? 0 : (uint8_t)chosen; // 0xFFFF => no report id

    // Pass 2: extract that report's fields.
    hid_descriptor_parse(desc_data, desc_len, tab_parse_field, conn);

    // Accept a relative mouse or an absolute digitizer/pen; reject an absolute
    // Generic-Desktop device with no digitizer usage (e.g. a gamepad's sticks).
    conn->valid = conn->x_size > 0 && conn->y_size > 0 &&
                  (conn->x_relative || conn->tip_offset != 0xFFFF ||
                   conn->inrange_offset != 0xFFFF);

    DBG("tab_mount: slot=%d, valid=%d, x_rel=%d, tip=%d\n",
        slot, conn->valid, conn->x_relative, conn->tip_offset != 0xFFFF);

    return conn->valid;
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
