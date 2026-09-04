/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/layout.h"
#include <stdio.h>

/* The image's header and record shape, as keyboard_layout_gen.py lays them
 * out. The header says where each record starts; a record's own fields
 * are fixed and are the offsets below. */
#define LAYOUT_MAGIC 0x4C4Bu

#define LAYOUT_OFF_NAME 0
#define LAYOUT_OFF_DESC (LAYOUT_OFF_NAME + LAYOUT_NAME_MAX / 2)
#define LAYOUT_OFF_CAPS (LAYOUT_OFF_DESC + LAYOUT_DESC_MAX / 2)
#define LAYOUT_OFF_D2N (LAYOUT_OFF_CAPS + 8)
#define LAYOUT_OFF_D3N (LAYOUT_OFF_D2N + 1)
#define LAYOUT_OFF_KEYS (LAYOUT_OFF_D3N + 1)
#define LAYOUT_OFF_DEAD (LAYOUT_OFF_KEYS + 128 * 4)

static bool layout_checked;
static int layout_layouts;

bool layout_init(void)
{
    layout_checked = true;
    if (layout_word(0) != LAYOUT_MAGIC)
    {
        /* Said here, where it is known, rather than by whoever called: a
         * platform that links the database in never reaches this, and one
         * that loads it wants to hear about it once, at boot. */
        printf("keyboard: no layouts\n");
        layout_layouts = 0;
        return false;
    }
    layout_layouts = layout_word(1);
    return true;
}

/* A platform that links the database in never fails, so it is spared
 * having to say so at boot; the first lookup reads the header. */
static void layout_ready(void)
{
    if (!layout_checked)
        layout_init();
}

// Word 0 of a layout's record, or 0 for a layout that is not there.
static uint32_t layout_record(int idx)
{
    layout_ready();
    if (idx < 0 || idx >= layout_layouts)
        return 0;
    return layout_word(2 + (uint32_t)idx);
}

int layout_count(void)
{
    layout_ready();
    return layout_layouts;
}

static void layout_string(uint32_t at, char *buf, unsigned size)
{
    if (!at)
    {
        buf[0] = 0;
        return;
    }
    for (unsigned i = 0; i < size; i += 2)
    {
        uint16_t w = layout_word(at + i / 2);
        buf[i] = (char)w;
        buf[i + 1] = (char)(w >> 8);
    }
    buf[size - 1] = 0;
}

void layout_name(int idx, char *buf)
{
    uint32_t rec = layout_record(idx);
    layout_string(rec ? rec + LAYOUT_OFF_NAME : 0, buf, LAYOUT_NAME_MAX);
}

void layout_description(int idx, char *buf)
{
    uint32_t rec = layout_record(idx);
    layout_string(rec ? rec + LAYOUT_OFF_DESC : 0, buf, LAYOUT_DESC_MAX);
}

uint16_t layout_code_point(int idx, uint8_t keycode, unsigned col)
{
    uint32_t rec = layout_record(idx);
    if (!rec)
        return 0;
    return layout_word(rec + LAYOUT_OFF_KEYS + (uint32_t)keycode * 4 + col);
}

bool layout_use_caps(int idx, uint8_t keycode)
{
    uint32_t rec = layout_record(idx);
    if (!rec)
        return false;
    return layout_word(rec + LAYOUT_OFF_CAPS + (keycode >> 4)) & (1 << (keycode & 15));
}

unsigned layout_dead2_count(int idx)
{
    uint32_t rec = layout_record(idx);
    return rec ? layout_word(rec + LAYOUT_OFF_D2N) : 0;
}

unsigned layout_dead3_count(int idx)
{
    uint32_t rec = layout_record(idx);
    return rec ? layout_word(rec + LAYOUT_OFF_D3N) : 0;
}

uint16_t layout_dead2(int idx, unsigned entry, unsigned field)
{
    uint32_t rec = layout_record(idx);
    if (!rec)
        return 0;
    return layout_word(rec + LAYOUT_OFF_DEAD + entry * 3 + field);
}

uint16_t layout_dead3(int idx, unsigned entry, unsigned field)
{
    uint32_t rec = layout_record(idx);
    if (!rec)
        return 0;
    uint32_t at = rec + LAYOUT_OFF_DEAD + (uint32_t)layout_word(rec + LAYOUT_OFF_D2N) * 3;
    return layout_word(at + entry * 4 + field);
}
