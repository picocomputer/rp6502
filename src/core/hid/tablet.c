/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/hid.h"
#include "core/hid/tablet.h"
#include "core/sys/debug_log.h"
#include "core/sys/xram.h"
#include "core/vga/vga.h"
#include "machine.h"
#include <string.h>

/* The XRAM report block, whose offsets are in tablet.h. Every field is one
 * byte, so each 6502 read is atomic. A coordinate too wide for one byte is
 * delivered as a set of single-byte windows of which exactly one is non-zero,
 * and the program decodes it by taking the first non-zero byte. An inactive
 * contact is all zero, which is flags of 0 and no window set. The wheel and
 * pan bytes are counters read by subtracting the value seen last, as the
 * mouse's are. The program owns the control byte, and it leads the block so
 * that everything the firmware writes back is one contiguous run. */
/* A relative mouse counts far finer than a canvas pixel, so it is tracked in a
 * fixed reference resolution at the same rate mouse.c reports at and then
 * scaled to the canvas. The program then reads an absolute position that moves
 * at the same speed whatever the canvas width is. */
#define TABLET_REF_WIDTH 640
#define TABLET_REF_HEIGHT 480
#define TABLET_MOUSE_DIV 2 /* counts per reference pixel, matching mouse.c */

static uint8_t tablet_state[TABLET_BLOCK_SIZE];
static uint16_t tablet_xram;

/* Kept apart from the block because tablet_xreg blanks the block, and a
 * machine that lends the program its own cursor has to say so again in the
 * status byte of a block the program has just moved. */
static bool tablet_host_cursor;

/* The pointer in canvas pixels, which is what is written to XRAM. */
static int16_t tablet_x;
static int16_t tablet_y;

/* A relative mouse's pointer in the TABLET_REF space, with the fraction of a
 * reference pixel carried between reports so that slow motion is not lost. */
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

/* X into three windows. A window byte carries 1 to 255, and 0 says the value
 * is not in that window, so three of them span 0 to 764. */
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

/* Y into two windows, spanning 0 to 509. */
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
    memset(&tablet_state[TABLET_OFF_CONTACTS + i * TABLET_CONTACT_SIZE], 0, TABLET_CONTACT_SIZE);
}

/* Everything the firmware owns runs contiguously after the program's control
 * byte, so one memcpy publishes all of it. A 6502 reading through the copy can
 * see a contact half updated, or flags from one frame with coordinates from
 * another; that costs one stale or blank frame, and the next report publishes
 * the whole block again. */
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

/* The whole block is saved, control byte included, but a load publishes only
 * from the status byte on, because the control byte is the program's. */
void tablet_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u16(c, tablet_xram);
    sst_put(c, tablet_state, sizeof tablet_state);
    sst_put_bool(c, tablet_host_cursor);
}

bool tablet_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint16_t at = sst_get_u16(c);
    uint8_t block[TABLET_BLOCK_SIZE];
    sst_get(c, block, sizeof block);
    bool cursor = sst_get_bool(c);
    if (!sst_ok(c))
        return false;
    tablet_xram = at;
    memcpy(tablet_state, block, sizeof tablet_state);
    tablet_host_cursor = cursor;
    tablet_write_xram();
    return true;
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
    if (tablet_xram != 0xFFFF) /* the one write that also seeds TABLET_CURSOR_OFF */
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
        RP6502_LOG(hid, INFO, "tablet mount slot=%d, x_rel=%d, tip=%d", slot, desc->x_relative,
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
    /* Contact 0 is released once the last pointer is gone, so that a button
     * held at unplug does not stay down. A pointer that is still plugged in
     * refreshes the contact on its next report. */
    for (int i = 0; i < TABLET_MAX_MICE; ++i)
        if (tablet_connections[i].valid)
            return true;
    tablet_clear_contact(0);
    tablet_write_xram();
    return true;
}

/* An axis whose declared logical minimum is below zero is signed. */
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

    if (conn->x_relative)
    {
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
        /* The width ratio is applied to both axes, so that a diagonal stays
         * at 45 degrees. The clamp below bounds the vertical extent. */
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

    /* An absolute device set tablet_x and tablet_y directly, so the reference
     * space is put back in step with them. A mouse moved afterwards then
     * carries on from here instead of snapping back to where it left off. */
    if (!conn->x_relative)
    {
        tablet_ref_x = (int16_t)((int32_t)tablet_x * TABLET_REF_WIDTH / cw);
        tablet_ref_y = (int16_t)((int32_t)tablet_y * TABLET_REF_WIDTH / cw);
        tablet_sub_x = tablet_sub_y = 0;
    }

    // A digitizer's Tip Switch is the primary button.
    uint8_t buttons = 0;
    for (int i = 0; i < 5; i++)
        if (conn->button_offsets[i] != 0xFFFF)
            if (hid_extract_bits(report_data, report_data_len, conn->button_offsets[i], 1))
                buttons |= (uint8_t)(1 << i);
    if (conn->tip_offset != 0xFFFF)
        if (hid_extract_bits(report_data, report_data_len, conn->tip_offset, 1))
            buttons |= TABLET_FLAG_LEFT;

    /* A mouse always hovers. An absolute pen hovers while it is In Range, and
     * a touchscreen that reports a tip and no In Range never hovers. */
    bool hover = conn->x_relative;
    if (!conn->x_relative && conn->inrange_offset != 0xFFFF)
        hover = hid_extract_bits(report_data, report_data_len, conn->inrange_offset, 1) != 0;

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

void tablet_host_pointer(int x, int y, uint8_t buttons, bool host_cursor)
{
    tablet_set_host_cursor(host_cursor);
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
