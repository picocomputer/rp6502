/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/hid.h"
#include "core/sys/div64.h"
#include "core/hid/tablet.h"
#include "core/hid/mouse.h"
#include "core/sys/debug_log.h"
#include "core/sys/xram.h"
#include "core/vga/vga.h"
#include "machine.h"
#include <string.h>

/* The offsets of the XRAM report block are in tablet.h. The wheel and pan
 * bytes are counters read by subtracting the previous value, as the mouse's
 * are. The program owns the control and status bytes, and they lead the
 * block so that everything the firmware writes back is one contiguous run. */
/* A mouse counts far finer than a canvas pixel, so the pointer it moves is
 * tracked in a fixed reference resolution, one pixel of it to one count of
 * the mouse's block (core/hid/mouse.h), and then scaled to the canvas. The
 * program then reads an absolute position that moves at the same speed
 * whatever the canvas width is. The reference space is always this wide,
 * and as tall as the canvas's shape makes it. */
#define TABLET_REF_WIDTH 640

static uint8_t tablet_state[TABLET_BLOCK_SIZE];
static uint16_t tablet_xram;

/* Whether the host draws the cursor, so the system draws none. */
static bool tablet_host_cursor;

/* Contact 0's buttons and hover as the host's own tablet has them
 * (tablet_host_contacts), and as the mouse has them (tablet_host_mouse).
 * They are merged with each digitizer's (tablet_merge_flags). */
static uint8_t tablet_host_flags;
static uint8_t tablet_mouse_flags;

/* The pointer in canvas pixels, which is what is written to XRAM. */
static int16_t tablet_x;
static int16_t tablet_y;

/* The pointer in the TABLET_REF space, with the fraction of a reference
 * pixel a mouse's motion leaves over, so that slow motion is not lost. */
static int16_t tablet_ref_x;
static int16_t tablet_ref_y;
static int32_t tablet_sub_x; /* Q16.16, as mouse_host_move takes */
static int32_t tablet_sub_y;

static tablet_connection_t tablet_connections[TABLET_MAX_MICE];

/* The canvas the pointer was last placed on. When the canvas changes, the
 * pointer keeps its place in the reference space and is put on the new one. */
static int16_t tablet_cw;
static int16_t tablet_ch;

/* The wheel and pan counters and contact 0, as the last tablet_commit left
 * them. They are one run of the block, from the wheel to contact 0's end. */
#define TABLET_COMMITTED_SIZE (TABLET_OFF_CONTACTS + TABLET_CONTACT_SIZE - TABLET_OFF_WHEEL)
static uint8_t tablet_committed[TABLET_COMMITTED_SIZE];

/* An address that would put the block past the end of XRAM would have the
 * next write of it go past it. A load is committed as it stands, so the next
 * change is measured from the loaded block. */
static bool tablet_sst_valid(sst_cursor_t *c)
{
    if (tablet_xram > 0x10000 - TABLET_BLOCK_SIZE && tablet_xram != 0xFFFF)
        return sst_refuse(c, "tablet block past the end of XRAM");
    memcpy(tablet_committed, &tablet_state[TABLET_OFF_WHEEL], TABLET_COMMITTED_SIZE);
    return true;
}

/* The whole block is saved, control byte included, and the pointer in the
 * reference space with its carry and the canvas it was placed on, which a
 * host's mouse moves on from (tablet_host_mouse), as the mouse saves its
 * own carry. The place on the canvas is rebuilt from them at the next
 * move. */
#define TABLET_SST(X)                                              \
    X(U16, tablet_xram)                                            \
    X(BYTES, tablet_state)                                         \
    X(SKIP, tablet_host_cursor, "set by every host pointer event") \
    X(U8, tablet_host_flags)                                       \
    X(U8, tablet_mouse_flags)                                      \
    X(VALIDATE, tablet_sst_valid)                                  \
    X(SKIP, tablet_x, "rebuilt from the reference space")         \
    X(SKIP, tablet_y, "rebuilt from the reference space")         \
    X(I16, tablet_ref_x)                                           \
    X(I16, tablet_ref_y)                                           \
    X(I32, tablet_sub_x)                                           \
    X(I32, tablet_sub_y)                                           \
    X(I16, tablet_cw)                                              \
    X(I16, tablet_ch)                                              \
    X(SKIP, tablet_committed, "rebuilt from tablet_state")         \
    X(SKIP, tablet_connections, "nothing mounts on a machine that saves")
SST_TABLE(tablet, TABLET_SST)

static tablet_connection_t *HOST_IN_RAM_FUNC tablet_get_connection_by_slot(int slot)
{
    for (int i = 0; i < TABLET_MAX_MICE; ++i)
        if (tablet_connections[i].valid && tablet_connections[i].slot == slot)
            return &tablet_connections[i];
    return NULL;
}

static int HOST_IN_RAM_FUNC tablet_clamp(int v)
{
    if (v < 0)
        return 0;
    if (v > 0xFFF)
        return 0xFFF;
    return v;
}

static void HOST_IN_RAM_FUNC tablet_put_contact(int i, uint8_t flags, int x, int y)
{
    uint8_t *c = &tablet_state[TABLET_OFF_CONTACTS + i * TABLET_CONTACT_SIZE];
    x = tablet_clamp(x);
    y = tablet_clamp(y);
    c[0] = flags;
    c[1] = (uint8_t)y;
    c[2] = (uint8_t)x;
    c[3] = (uint8_t)((y >> 8) << 4 | x >> 8);
}

static void tablet_clear_contact(int i)
{
    tablet_state[TABLET_OFF_CONTACTS + i * TABLET_CONTACT_SIZE] = 0;
}

/* A contact's place, as tablet_put_contact packs it. */
static int16_t HOST_IN_RAM_FUNC tablet_contact_x(const uint8_t *c)
{
    return (int16_t)(c[2] | (c[3] & 0x0F) << 8);
}

static int16_t HOST_IN_RAM_FUNC tablet_contact_y(const uint8_t *c)
{
    return (int16_t)(c[1] | (c[3] >> 4) << 8);
}

/* Every change to the tablet ends here, mapped or not. by_user is false for a
 * pointer the firmware moved, as onto a new canvas. The owner hears of a
 * change to contact 0 or the counters, measured from the last commit. The
 * block is written from the wheel on, so the program's control and status
 * are never written over. A 6502 reading the block during this memcpy can
 * get bytes from two reports, so a program reads the block until two reads
 * match. */
static void HOST_IN_RAM_FUNC tablet_commit(bool by_user)
{
    const uint8_t *now = &tablet_state[TABLET_OFF_WHEEL];
    if (memcmp(tablet_committed, now, TABLET_COMMITTED_SIZE))
    {
        const uint8_t *was = tablet_committed;
        const uint8_t *c_was = &was[TABLET_OFF_CONTACTS - TABLET_OFF_WHEEL];
        const uint8_t *c_now = &now[TABLET_OFF_CONTACTS - TABLET_OFF_WHEEL];
        const bool moved = memcmp(&c_was[1], &c_now[1], TABLET_CONTACT_SIZE - 1) != 0;
        tablet_on_contact(c_was[0], c_now[0], tablet_contact_x(c_now), tablet_contact_y(c_now),
                          (int8_t)(now[0] - was[0]), (int8_t)(now[1] - was[1]),
                          by_user && moved);
        memcpy(tablet_committed, now, TABLET_COMMITTED_SIZE);
    }
    if (tablet_xram != 0xFFFF)
        memcpy((uint8_t *)&xram[tablet_xram + TABLET_OFF_WHEEL], now,
               TABLET_BLOCK_SIZE - TABLET_OFF_WHEEL);
}

void HOST_IN_FLASH_FUNC tablet_init(void)
{
    tablet_host_flags = tablet_mouse_flags = 0;
    tablet_stop();
}

void tablet_stop(void)
{
    tablet_xram = 0xFFFF;
}

/* The contacts and counters are kept whether a program has the tablet or
 * not, as the mouse's are, so a program that maps it finds a mouse already
 * hovering where it is. Mapping seeds the program's two bytes: the arrow,
 * and the tablet shared. */
bool HOST_IN_FLASH_FUNC tablet_xreg(uint16_t word)
{
    bool mapped = word <= 0x10000 - TABLET_BLOCK_SIZE;
    tablet_xram = mapped ? word : 0xFFFF;
    tablet_state[TABLET_OFF_CONTROL] = TABLET_CURSOR_ARROW;
    tablet_state[TABLET_OFF_STATUS] = TABLET_STATUS_SHARED;
    if (tablet_xram != 0xFFFF) /* the one write of the program's bytes */
        memcpy((uint8_t *)&xram[tablet_xram], tablet_state, TABLET_BLOCK_SIZE);
    return mapped || word == 0xFFFF;
}

/* Contact 0 is the one pointer that every device moves, so its flags are the
 * buttons and hover of all of them, as the mouse merges its buttons. */
static void HOST_IN_RAM_FUNC tablet_merge_flags(void)
{
    uint8_t merged = tablet_host_flags | tablet_mouse_flags;
    for (int i = 0; i < TABLET_MAX_MICE; ++i)
        if (tablet_connections[i].valid)
            merged |= tablet_connections[i].flags;
    tablet_state[TABLET_OFF_CONTACTS] = merged;
}

bool tablet_mount(int slot, const tablet_connection_t *desc)
{
    if (!desc->valid)
        return false;
    for (int i = 0; i < TABLET_MAX_MICE; ++i)
    {
        if (tablet_connections[i].valid)
            continue;
        tablet_connections[i] = *desc;
        tablet_connections[i].slot = slot;
        /* An absolute mouse hovers from the moment it is plugged in, before
         * it moves, having no tip or In Range to say otherwise. */
        const bool hover = desc->tip_offset == HID_ABSENT && desc->inrange_offset == HID_ABSENT;
        tablet_connections[i].flags = hover ? TABLET_FLAG_HOVER : 0;
        tablet_merge_flags();
        tablet_commit(true);
        RP6502_LOG(hid, INFO, "tablet mount slot=%d, tip=%d", slot, desc->tip_offset != HID_ABSENT);
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
    /* A button held at unplug is let go at once, and the pointers still
     * plugged in keep theirs. */
    tablet_merge_flags();
    tablet_commit(true);
    return true;
}

/* An axis whose declared logical minimum is below zero is signed. */
static int32_t HOST_IN_RAM_FUNC tablet_axis_value(const uint8_t *r, uint16_t len, uint16_t off, uint8_t size, int32_t lmin)
{
    if (lmin < 0)
        return hid_extract_signed(r, len, off, size);
    return (int32_t)hid_extract_bits(r, len, off, size);
}

/* A place on the canvas is a small number, and one past it is off the
 * canvas however far, so the divide that finds it is the short one: 15 bits
 * of answer, which is more than any canvas, and no call to the compiler's
 * 64-bit divide. */
static int64_t HOST_IN_RAM_FUNC tablet_divide(int64_t n, int64_t d)
{
    return div64_small((uint64_t)n, (uint64_t)d, 15);
}

/* An absolute device's report to canvas pixels, rounded down, and false for a
 * touch off the canvas. A touchscreen covers the display, so a touch is placed
 * on the picture and the canvas is found in it: the canvas fills the picture's
 * width and is centred down it. A pen's tablet keeps its shape, so a circle
 * traced on it is a circle on the canvas: one scale serves both axes, the
 * largest that still reaches every pixel, with the canvas centred on the
 * tablet and the tablet's spare strip held at the canvas's edge. */
static bool HOST_IN_RAM_FUNC tablet_place(const tablet_connection_t *conn,
                                          int32_t rx, int32_t ry, int cw, int ch)
{
    const int64_t xs = conn->x_max - conn->x_min;
    const int64_t ys = conn->y_max - conn->y_min;
    if (xs <= 0 || ys <= 0)
        return true;
    const int64_t dx = rx - conn->x_min;
    const int64_t dy = ry - conn->y_min;
    int64_t x, y;
    if (conn->touchscreen)
    {
        /* The logical range is xs + 1 by ys + 1 cells across the display,
         * so the last cell is still on it. The canvas is dw * ch / cw lines
         * of the display's dh, centred. */
        int dw, dh;
        vga_display_size(&dw, &dh);
        const int64_t xn = xs + 1, yn = ys + 1;
        x = dx < 0 ? 0 : tablet_divide(dx * cw, xn);
        int64_t num = 2 * dy * dh * cw - ((int64_t)dh * cw - (int64_t)ch * dw) * yn;
        if (num < 0)
            return false;
        y = tablet_divide(num, 2 * yn * dw);
        if (y >= ch)
            return false;
    }
    else
    {
        /* The tablet's width and height, from its physical extent where the
         * descriptor gives one, else from its logical extent. */
        const int64_t pw = conn->x_phys > 0 ? conn->x_phys : xs;
        const int64_t ph = conn->y_phys > 0 ? conn->y_phys : ys;
        /* Canvas pixels per unit of tablet, kn / kd, the width's or the
         * height's, whichever is larger. */
        int64_t kn = cw, kd = pw;
        if ((int64_t)ch * pw > (int64_t)cw * ph)
            kn = ch, kd = ph;
        int64_t nx = (2 * dx - xs) * pw * kn + (int64_t)cw * xs * kd;
        int64_t ny = (2 * dy - ys) * ph * kn + (int64_t)ch * ys * kd;
        x = nx < 0 ? 0 : tablet_divide(nx, 2 * xs * kd);
        y = ny < 0 ? 0 : tablet_divide(ny, 2 * ys * kd);
    }
    tablet_x = (int16_t)(x > cw - 1 ? cw - 1 : x);
    tablet_y = (int16_t)(y > ch - 1 ? ch - 1 : y);
    return true;
}

/* The pointer on a cw by ch canvas, from its place in the reference space,
 * which has the canvas's shape, so the pointer stops at the bottom of a
 * widescreen canvas the moment it reaches it, and the first move up is seen
 * at once rather than after a band of travel below the canvas. The width
 * ratio is applied to both axes, so that a diagonal stays at 45 degrees. */
static void HOST_IN_RAM_FUNC tablet_from_ref(int cw, int ch)
{
    const int ref_h = ch * TABLET_REF_WIDTH / cw;
    if (tablet_ref_x < 0)
        tablet_ref_x = 0;
    else if (tablet_ref_x > TABLET_REF_WIDTH - 1)
        tablet_ref_x = TABLET_REF_WIDTH - 1;
    if (tablet_ref_y < 0)
        tablet_ref_y = 0;
    else if (tablet_ref_y > ref_h - 1)
        tablet_ref_y = (int16_t)(ref_h - 1);
    tablet_x = (int16_t)((int32_t)tablet_ref_x * cw / TABLET_REF_WIDTH);
    tablet_y = (int16_t)((int32_t)tablet_ref_y * cw / TABLET_REF_WIDTH);
    tablet_cw = (int16_t)cw;
    tablet_ch = (int16_t)ch;
}

/* A tablet puts the pointer straight on the canvas, so the reference space
 * is put back in step with it. A mouse moved afterwards then carries on from
 * here instead of snapping back to where it left off. */
static void HOST_IN_RAM_FUNC tablet_to_ref(int cw, int ch)
{
    tablet_ref_x = (int16_t)((int32_t)tablet_x * TABLET_REF_WIDTH / cw);
    tablet_ref_y = (int16_t)((int32_t)tablet_y * TABLET_REF_WIDTH / cw);
    tablet_sub_x = tablet_sub_y = 0;
    tablet_cw = (int16_t)cw;
    tablet_ch = (int16_t)ch;
}

void HOST_IN_RAM_FUNC tablet_report(int slot, uint8_t const *data, size_t size)
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

    /* A touch off the canvas, on the band of a touchscreen's display the
     * canvas does not fill, is no touch at all. */
    int32_t rx = tablet_axis_value(report_data, report_data_len, conn->x_offset, conn->x_size, conn->x_min);
    int32_t ry = tablet_axis_value(report_data, report_data_len, conn->y_offset, conn->y_size, conn->y_min);
    const bool on_canvas = tablet_place(conn, rx, ry, cw, ch);
    if (!on_canvas)
    {
        conn->flags = 0;
        tablet_merge_flags();
        tablet_commit(true);
        return;
    }
    tablet_to_ref(cw, ch);

    // A digitizer's Tip Switch is the primary button.
    uint8_t buttons = 0;
    for (int i = 0; i < 5; i++)
        if (conn->button_offsets[i] != 0xFFFF)
            if (hid_extract_bits(report_data, report_data_len, conn->button_offsets[i], 1))
                buttons |= (uint8_t)(1 << i);
    if (conn->tip_offset != 0xFFFF)
        if (hid_extract_bits(report_data, report_data_len, conn->tip_offset, 1))
            buttons |= TABLET_FLAG_LEFT;

    /* An absolute mouse always hovers, having no tip. A pen hovers while it
     * is In Range, and a touchscreen that reports a tip and no In Range never
     * hovers. */
    bool hover = conn->tip_offset == 0xFFFF;
    if (conn->inrange_offset != 0xFFFF)
        hover = hid_extract_bits(report_data, report_data_len, conn->inrange_offset, 1) != 0;

    if (conn->wheel_size > 0)
        tablet_state[TABLET_OFF_WHEEL] += hid_extract_signed(report_data, report_data_len,
                                                             conn->wheel_offset, conn->wheel_size);
    if (conn->pan_size > 0)
        tablet_state[TABLET_OFF_PAN] += hid_extract_signed(report_data, report_data_len,
                                                           conn->pan_offset, conn->pan_size);

    conn->flags = (uint8_t)(buttons | (hover ? TABLET_FLAG_HOVER : 0));
    tablet_put_contact(0, 0, tablet_x, tablet_y);
    tablet_merge_flags();
    tablet_commit(true);
}

bool tablet_is_mapped(void)
{
    return tablet_xram != 0xFFFF;
}

bool HOST_IN_RAM_FUNC tablet_shared(void)
{
    return tablet_xram == 0xFFFF || (xram[tablet_xram + TABLET_OFF_STATUS] & TABLET_STATUS_SHARED);
}

bool tablet_wanted(void)
{
    return tablet_is_mapped() || tablet_claimed();
}

bool tablet_wheel_wanted(void)
{
    return tablet_is_mapped() || tablet_wheel_claimed();
}

void HOST_IN_RAM_FUNC tablet_host_contacts(const tablet_contact_t *at, int n, bool host_cursor)
{
    if (n > TABLET_MAX_CONTACTS)
        n = TABLET_MAX_CONTACTS;
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    for (int i = 0; i < n; ++i)
    {
        const int x = at[i].x < 0 ? 0 : at[i].x > cw - 1 ? cw - 1 : at[i].x;
        const int y = at[i].y < 0 ? 0 : at[i].y > ch - 1 ? ch - 1 : at[i].y;
        tablet_put_contact(i, at[i].flags, x, y);
        if (i == 0)
        {
            tablet_x = (int16_t)x;
            tablet_y = (int16_t)y;
            tablet_to_ref(cw, ch);
        }
    }
    for (int i = n; i < TABLET_MAX_CONTACTS; ++i)
        tablet_clear_contact(i);
    if (n > 0)
        tablet_host_cursor = host_cursor;
    tablet_host_flags = n > 0 ? at[0].flags : 0;
    tablet_merge_flags();
    tablet_commit(true);
}

void HOST_IN_RAM_FUNC tablet_host_mouse(int32_t dx, int32_t dy, uint8_t buttons)
{
    tablet_sub_x += dx;
    tablet_sub_y += dy;
    const int ix = tablet_sub_x / MOUSE_ONE;
    const int iy = tablet_sub_y / MOUSE_ONE;
    tablet_sub_x -= ix * MOUSE_ONE;
    tablet_sub_y -= iy * MOUSE_ONE;
    tablet_ref_x = (int16_t)(tablet_ref_x + ix);
    tablet_ref_y = (int16_t)(tablet_ref_y + iy);
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    tablet_from_ref(cw, ch);
    tablet_host_cursor = false;
    tablet_mouse_flags = (uint8_t)((buttons & TABLET_FLAG_BUTTONS) | TABLET_FLAG_HOVER);
    tablet_put_contact(0, 0, tablet_x, tablet_y);
    tablet_merge_flags();
    tablet_commit(true);
}

void HOST_IN_RAM_FUNC tablet_host_mouse_gone(void)
{
    tablet_mouse_flags = 0;
    tablet_merge_flags();
    tablet_commit(true);
}

void HOST_IN_RAM_FUNC tablet_host_wheel(int dwheel, int dpan)
{
    if (dwheel == 0 && dpan == 0)
        return;
    tablet_state[TABLET_OFF_WHEEL] = (uint8_t)(tablet_state[TABLET_OFF_WHEEL] + dwheel);
    tablet_state[TABLET_OFF_PAN] = (uint8_t)(tablet_state[TABLET_OFF_PAN] + dpan);
    tablet_commit(true);
}

void tablet_task(void)
{
    /* A new canvas puts the pointer on it, where it was in the reference
     * space, so it is never off the canvas a program reads it on. */
    int cw, ch;
    vga_canvas_size(&cw, &ch);
    if (cw != tablet_cw || ch != tablet_ch)
    {
        /* The first canvas, at boot, has the pointer at its middle. */
        if (!tablet_cw)
        {
            tablet_ref_x = TABLET_REF_WIDTH / 2;
            tablet_ref_y = (int16_t)(ch * TABLET_REF_WIDTH / cw / 2);
        }
        /* The place in the reference space is kept as it is, so a pointer
         * low on a 512-line console is held at the bottom of a shorter canvas
         * and back where it was when the console returns. The next move of
         * a mouse starts from the edge it is shown on. */
        const int16_t ref_y = tablet_ref_y;
        tablet_from_ref(cw, ch);
        tablet_ref_y = ref_y;
        uint8_t *c0 = &tablet_state[TABLET_OFF_CONTACTS];
        tablet_put_contact(0, c0[0], tablet_x, tablet_y);
        tablet_commit(false);
    }
    const uint8_t *c = &tablet_state[TABLET_OFF_CONTACTS];
    uint8_t shape = tablet_shape();
    if (!(c[0] & TABLET_FLAG_HOVER))
        shape = TABLET_CURSOR_OFF;
    vga_set_pointer(shape, tablet_contact_x(c), tablet_contact_y(c));
}

bool tablet_host_cursor_shown(void)
{
    return tablet_host_cursor;
}

uint8_t tablet_shape(void)
{
    if (tablet_xram == 0xFFFF)
        return tablet_claimed() ? TABLET_CURSOR_ARROW : TABLET_CURSOR_OFF;
    return xram[tablet_xram + TABLET_OFF_CONTROL];
}
