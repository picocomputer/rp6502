/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/kbl.h"

/* The image's header and record shape, as kbd_layout_gen.py lays them
 * out. The header says where each record starts; a record's own fields
 * are fixed and are the offsets below. */
#define KBL_MAGIC 0x4C4Bu

#define KBL_OFF_NAME 0
#define KBL_OFF_DESC (KBL_OFF_NAME + KBL_NAME_MAX / 2)
#define KBL_OFF_CAPS (KBL_OFF_DESC + KBL_DESC_MAX / 2)
#define KBL_OFF_D2N (KBL_OFF_CAPS + 8)
#define KBL_OFF_D3N (KBL_OFF_D2N + 1)
#define KBL_OFF_KEYS (KBL_OFF_D3N + 1)
#define KBL_OFF_DEAD (KBL_OFF_KEYS + 128 * 4)

static bool kbl_checked;
static int kbl_layouts;

bool kbl_init(void)
{
    kbl_checked = true;
    if (kbl_word(0) != KBL_MAGIC)
    {
        kbl_layouts = 0;
        return false;
    }
    kbl_layouts = kbl_word(1);
    return true;
}

/* A platform that links the database in never fails, so it is spared
 * having to say so at boot; the first lookup reads the header. */
static void kbl_ready(void)
{
    if (!kbl_checked)
        kbl_init();
}

// Word 0 of a layout's record, or 0 for a layout that is not there.
static uint32_t kbl_record(int layout)
{
    kbl_ready();
    if (layout < 0 || layout >= kbl_layouts)
        return 0;
    return kbl_word(2 + (uint32_t)layout);
}

int kbl_count(void)
{
    kbl_ready();
    return kbl_layouts;
}

static void kbl_string(uint32_t at, char *buf, unsigned size)
{
    if (!at)
    {
        buf[0] = 0;
        return;
    }
    for (unsigned i = 0; i < size; i += 2)
    {
        uint16_t w = kbl_word(at + i / 2);
        buf[i] = (char)w;
        buf[i + 1] = (char)(w >> 8);
    }
    buf[size - 1] = 0;
}

void kbl_name(int layout, char *buf)
{
    uint32_t rec = kbl_record(layout);
    kbl_string(rec ? rec + KBL_OFF_NAME : 0, buf, KBL_NAME_MAX);
}

void kbl_description(int layout, char *buf)
{
    uint32_t rec = kbl_record(layout);
    kbl_string(rec ? rec + KBL_OFF_DESC : 0, buf, KBL_DESC_MAX);
}

uint16_t kbl_code_point(int layout, uint8_t keycode, unsigned col)
{
    uint32_t rec = kbl_record(layout);
    if (!rec)
        return 0;
    return kbl_word(rec + KBL_OFF_KEYS + (uint32_t)keycode * 4 + col);
}

bool kbl_use_caps(int layout, uint8_t keycode)
{
    uint32_t rec = kbl_record(layout);
    if (!rec)
        return false;
    return kbl_word(rec + KBL_OFF_CAPS + (keycode >> 4)) & (1 << (keycode & 15));
}

unsigned kbl_dead2_count(int layout)
{
    uint32_t rec = kbl_record(layout);
    return rec ? kbl_word(rec + KBL_OFF_D2N) : 0;
}

unsigned kbl_dead3_count(int layout)
{
    uint32_t rec = kbl_record(layout);
    return rec ? kbl_word(rec + KBL_OFF_D3N) : 0;
}

uint16_t kbl_dead2(int layout, unsigned entry, unsigned field)
{
    uint32_t rec = kbl_record(layout);
    if (!rec)
        return 0;
    return kbl_word(rec + KBL_OFF_DEAD + entry * 3 + field);
}

uint16_t kbl_dead3(int layout, unsigned entry, unsigned field)
{
    uint32_t rec = kbl_record(layout);
    if (!rec)
        return 0;
    uint32_t at = rec + KBL_OFF_DEAD + (uint32_t)kbl_word(rec + KBL_OFF_D2N) * 3;
    return kbl_word(at + entry * 4 + field);
}
