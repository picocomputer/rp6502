/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/sst.h"
#include "core/hid/layout.h"
#include "core/hid/keymap.h"
#include "core/str/str.h"
#include "core/sys/config.h"
#include "machine.h"
#include <stdio.h>
#include <string.h>
/* strcasecmp, which matches a layout name, is named by POSIX rather than by C.
 * A host whose compiler has no such header supplies one; see src/osal. */
#include <strings.h>

#include "layout_db.h"

int layout_count(void)
{
    return LAYOUT_DB_LAYOUTS;
}

const char *layout_name(int idx)
{
    return layout_db[idx].name;
}

const char *layout_description(int idx)
{
    return layout_db[idx].description;
}

uint16_t HOST_IN_RAM_FUNC layout_code_point(int idx, uint8_t keycode, unsigned col)
{
    return layout_db[idx].keys[keycode][col];
}

bool HOST_IN_RAM_FUNC layout_use_caps(int idx, uint8_t keycode)
{
    return layout_db[idx].caps[keycode >> 4] & (1 << (keycode & 15));
}

unsigned layout_dead2_count(int idx)
{
    return layout_db[idx].dead2_count;
}

unsigned layout_dead3_count(int idx)
{
    return layout_db[idx].dead3_count;
}

uint16_t layout_dead2(int idx, unsigned entry, unsigned field)
{
    return layout_db[idx].dead[entry * 3 + field];
}

uint16_t layout_dead3(int idx, unsigned entry, unsigned field)
{
    const layout_db_t *lay = &layout_db[idx];
    return lay->dead[lay->dead2_count * 3 + entry * 4 + field];
}

/* The user's list. The position is the layout in use, and points into
 * config's own storage. */
static const char *layout_list_pos;

// The layout in use, or -1 before the list is first applied.
static int layout_list_index = -1;

/* A walk of the setting's list, which the session's config owns. */
#define LAYOUT_SST(X)                                           \
    X(SKIP, layout_list_pos)                                    \
    X(SKIP, layout_list_index)
SST_TABLE(layout, LAYOUT_SST)

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
    for (int i = 0; i < LAYOUT_DB_LAYOUTS; i++)
    {
        const char *name = layout_db[i].name;
        if (strlen(name) == len && !strncmp(layout_list_pos, name, len))
        {
            keymap_use_layout(i);
            layout_list_index = i;
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
        const char *name = NULL;
        for (int i = 0; i < LAYOUT_DB_LAYOUTS && !name; i++)
            if (strlen(layout_db[i].name) == tok_len &&
                !strncasecmp(in, layout_db[i].name, tok_len))
                name = layout_db[i].name;
        if (!name || layout_list_find(out, name))
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
    layout_list_pos = layout_list_index < 0 ? NULL
        : layout_list_find(layout_get_list(), layout_db[layout_list_index].name);
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
                 layout_db[layout_list_index].name,
                 layout_db[layout_list_index].description);
    return -1;
}

/* The width the caller asks for is ignored, because the lines are laid out
 * from the longest layout name. */
int HOST_IN_FLASH_FUNC layout_help_response(char *buf, size_t buf_size,
                                            int state, unsigned width)
{
    (void)width;
    if (state < 0 || state >= LAYOUT_DB_LAYOUTS)
        return -1;
    int maxlen = 0;
    for (int i = 0; i < LAYOUT_DB_LAYOUTS; i++)
    {
        int thislen = strlen(layout_db[i].name);
        if (thislen > maxlen)
            maxlen = thislen;
    }
    snprintf(buf, buf_size, "  %*s - \a%s\n", maxlen,
             layout_db[state].name, layout_db[state].description);
    return state + 1;
}

void HOST_IN_FLASH_FUNC layout_list_init(void)
{
    /* A machine with no stored list adopts US and writes it back, so the
     * config file completes itself once. */
    if (!layout_get_list()[0])
        layout_set_list("US");
    // A list the config file loaded has not been applied.
    if (!layout_list_pos)
        layout_apply_list(layout_get_list(), true);
}
