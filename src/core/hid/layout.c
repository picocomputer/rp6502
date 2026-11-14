/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/layout.h"
#include "core/hid/keymap.h"
#include "core/str/str.h"
#include "core/sys/config.h"
#include "core/sys/debug_log.h"
#include "machine.h"
#include <stdio.h>
#include <string.h>
/* strcasecmp, which matches a layout name, is named by POSIX rather than by C.
 * A host whose compiler has no such header supplies one; see src/osal. */
#include <strings.h>

/* The image's header and record shape, as layout.py lays them out. The
 * header says where each record starts, and the offsets below are fixed
 * within a record. */
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

bool HOST_IN_FLASH_FUNC layout_init(void)
{
    layout_checked = true;
    if (layout_word(0) != LAYOUT_MAGIC)
    {
        RP6502_LOG(keyboard, ERROR, "no layouts");
        layout_layouts = 0;
        return false;
    }
    layout_layouts = layout_word(1);
    return true;
}

/* A lookup can come before layout_init, as when the config file is read, so
 * the first one reads the header. */
static void HOST_IN_RAM_FUNC layout_ready(void)
{
    if (!layout_checked)
        layout_init();
}

// Where a layout's record starts, or 0 for a layout that is not there.
static uint32_t HOST_IN_RAM_FUNC layout_record(int idx)
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

uint16_t HOST_IN_RAM_FUNC layout_code_point(int idx, uint8_t keycode, unsigned col)
{
    uint32_t rec = layout_record(idx);
    if (!rec)
        return 0;
    return layout_word(rec + LAYOUT_OFF_KEYS + (uint32_t)keycode * 4 + col);
}

bool HOST_IN_RAM_FUNC layout_use_caps(int idx, uint8_t keycode)
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

/* The user's list. The position is the layout in use, and points into
 * config's own storage. */
static const char *layout_list_pos;

/* Copies of the name and description of the layout in use, because the
 * database may not be addressable memory. */
static char layout_list_name[LAYOUT_NAME_MAX];
static char layout_list_description[LAYOUT_DESC_MAX];

static size_t layout_list_token(const char *s)
{
    size_t len = 0;
    while (s[len] && s[len] != ' ')
        len++;
    return len;
}

// Types through the layout the position names.
static void layout_list_use(void)
{
    size_t len = layout_list_token(layout_list_pos);
    for (int i = 0; i < layout_count(); i++)
    {
        char name[LAYOUT_NAME_MAX];
        layout_name(i, name);
        if (strlen(name) == len && !strncmp(layout_list_pos, name, len))
        {
            keymap_use_layout(i);
            strcpy(layout_list_name, name);
            layout_description(i, layout_list_description);
            return;
        }
    }
}

void HOST_IN_FLASH_FUNC layout_list_cycle(void)
{
    const char *p = layout_list_pos + layout_list_token(layout_list_pos);
    while (*p == ' ')
        p++;
    if (!*p)
        p = layout_get_list();
    if (p == layout_list_pos)
        return;
    layout_list_pos = p;
    layout_list_use();
}

// Find name as a whole token in a space separated list.
static const char *layout_list_find(const char *list, const char *name)
{
    size_t name_len = strlen(name);
    while (*list)
    {
        while (*list == ' ')
            list++;
        if (!*list)
            break;
        size_t len = layout_list_token(list);
        if (len == name_len && !strncmp(list, name, len))
            return list;
        list += len;
    }
    return NULL;
}

bool layout_check_list(const char *in, char *out)
{
    size_t len = 0;
    out[0] = 0;
    while (*in)
    {
        while (*in == ' ')
            in++;
        if (!*in)
            break;
        size_t tok_len = layout_list_token(in);
        char name[LAYOUT_NAME_MAX];
        name[0] = 0;
        for (int i = 0; i < layout_count(); i++)
        {
            layout_name(i, name);
            if (strlen(name) == tok_len && !strncasecmp(in, name, tok_len))
                break;
            name[0] = 0;
        }
        if (!name[0] || layout_list_find(out, name))
            return false;
        if (len + (len ? 1 : 0) + strlen(name) + 1 > LAYOUT_LIST_SIZE)
            return false;
        if (len)
            out[len++] = ' ';
        strcpy(out + len, name);
        len += strlen(name);
        in += tok_len;
    }
    return len != 0;
}

/* The layout in use is kept if it survived the new list, and otherwise the
 * first is taken. Config's storage holds the new list by the time this
 * runs. */
void layout_apply_list(const char *list, bool changed)
{
    (void)list;
    (void)changed;
    layout_list_pos = layout_list_find(layout_get_list(), layout_list_name);
    if (!layout_list_pos)
        layout_list_pos = layout_get_list();
    layout_list_use();
}

int HOST_IN_FLASH_FUNC layout_list_response(char *buf, size_t buf_size,
                                            int state, unsigned width)
{
    (void)state;
    (void)width;
    const char *list = layout_get_list();
    if (strchr(list, ' '))
        snprintf(buf, buf_size, STR_SET_KB_LIST_RESPONSE, list);
    else
        snprintf(buf, buf_size, STR_SET_KB_RESPONSE,
                 layout_list_name, layout_list_description);
    return -1;
}

/* The width the caller asks for is ignored, because the lines are laid out
 * from the longest layout name. */
int HOST_IN_FLASH_FUNC layout_help_response(char *buf, size_t buf_size,
                                            int state, unsigned width)
{
    (void)width;
    if (state < 0 || state >= layout_count())
        return -1;
    char name[LAYOUT_NAME_MAX];
    char desc[LAYOUT_DESC_MAX];
    int maxlen = 0;
    for (int i = 0; i < layout_count(); i++)
    {
        layout_name(i, name);
        int thislen = strlen(name);
        if (thislen > maxlen)
            maxlen = thislen;
    }
    layout_name(state, name);
    layout_description(state, desc);
    snprintf(buf, buf_size, "  %*s - \a%s\n", maxlen, name, desc);
    return state + 1;
}

void HOST_IN_FLASH_FUNC layout_list_init(void)
{
    /* A machine with no stored list adopts US, or the first layout of a
     * database without it, and writes it back, so the config file completes
     * itself once. */
    if (!layout_get_list()[0])
    {
        int index = 0;
        char name[LAYOUT_NAME_MAX];
        for (int i = 0; i < layout_count(); i++)
        {
            layout_name(i, name);
            if (!strcmp(name, "US"))
                index = i;
        }
        layout_name(index, name);
        layout_set_list(name);
    }
    /* A list the config file loaded, or the empty one a machine with no
     * database is left with, has not been applied. */
    if (!layout_list_pos)
        layout_apply_list(layout_get_list(), true);
}
