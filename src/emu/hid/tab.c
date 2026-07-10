/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/hid/tab.h"
#include "emu/sys/mem.h"
#include <string.h>

/* Our copy of the report block. Offset TAB_OFF_CONTROL is ROM-owned and never
 * written back after the one-time init, so a whole-block memcpy is avoided on
 * update (it would clobber the ROM's cursor request). */
static uint8_t tab_block[TAB_BLOCK_SIZE];
static uint16_t tab_xram = 0xFFFF; /* 0xFFFF = not mapped */

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
    uint8_t *c = &tab_block[TAB_OFF_CONTACTS + i * TAB_CONTACT_SIZE];
    c[0] = flags;
    tab_encode_x(&c[1], x);
    tab_encode_y(&c[4], y);
}

static void tab_clear_contact(int i)
{
    tab_put_contact(i, 0, 0, 0); /* flags=0 marks it inactive; X/Y stay in-canvas */
}

/* Push status + contacts to XRAM, but never the ROM-owned control byte. */
static void tab_write_xram(void)
{
    if (tab_xram == 0xFFFF)
        return;
    xram[tab_xram + TAB_OFF_STATUS] = tab_block[TAB_OFF_STATUS];
    memcpy(&xram[tab_xram + TAB_OFF_CONTACTS], &tab_block[TAB_OFF_CONTACTS],
           TAB_MAX_CONTACTS * TAB_CONTACT_SIZE);
}

bool tab_set_xram(uint16_t addr)
{
    if (addr != 0xFFFF && addr > 0x10000 - TAB_BLOCK_SIZE)
        return false;
    tab_xram = addr;
    memset(tab_block, 0, sizeof(tab_block));
    tab_block[TAB_OFF_STATUS] = TAB_STATUS_HOST_CURSOR; /* desktop default; a touch clears it */
    for (int i = 0; i < TAB_MAX_CONTACTS; ++i)
        tab_clear_contact(i);
    if (tab_xram != 0xFFFF) /* one-time full write also seeds control=0 (ROM draws its own) */
        memcpy(&xram[tab_xram], tab_block, TAB_BLOCK_SIZE);
    return true;
}

bool tab_is_mapped(void)
{
    return tab_xram != 0xFFFF;
}

void tab_host_pointer(int x, int y, uint8_t buttons)
{
    tab_block[TAB_OFF_STATUS] |= TAB_STATUS_HOST_CURSOR;
    tab_put_contact(0, (uint8_t)(buttons | TAB_FLAG_HOVER), x, y);
    for (int i = 1; i < TAB_MAX_CONTACTS; ++i)
        tab_clear_contact(i);
    tab_write_xram();
}

void tab_host_touch(const tab_point_t *pts, int n)
{
    tab_block[TAB_OFF_STATUS] &= (uint8_t)~TAB_STATUS_HOST_CURSOR; /* a finger has no cursor */
    if (n > TAB_MAX_CONTACTS)
        n = TAB_MAX_CONTACTS;
    for (int i = 0; i < n; ++i)
        tab_put_contact(i, TAB_FLAG_LEFT, pts[i].x, pts[i].y); /* tip down, no hover */
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

uint8_t tab_control(void)
{
    if (tab_xram == 0xFFFF)
        return TAB_CURSOR_OFF;
    return xram[tab_xram + TAB_OFF_CONTROL];
}

void tab_reset(void)
{
    tab_xram = 0xFFFF;
    memset(tab_block, 0, sizeof(tab_block));
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* The web shell reads this to drop the mouse "click to capture" hint once a
 * program maps the tablet (the tablet takes the pointer without capturing it). */
EMSCRIPTEN_KEEPALIVE int tab_mapped(void)
{
    return tab_is_mapped() ? 1 : 0;
}
#endif
