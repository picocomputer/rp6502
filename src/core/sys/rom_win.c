/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See rom_win.h.
 */

#include "core/sys/rom_win.h"

#include <stdio.h> /* SEEK_SET and friends */

int rom_win_alloc(const rom_win_pool_t *p, uint32_t base, uint32_t len, int fd,
                  api_errno *err)
{
    for (int i = 0; i < p->max; i++)
        if (!p->slots[i].used)
        {
            p->slots[i] = (rom_win_t){.used = true, .base = base, .len = len, .fd = fd};
            return i;
        }
    *err = API_EMFILE;
    return -1;
}

rom_win_t *rom_win_get(const rom_win_pool_t *p, int desc)
{
    if (desc < 0 || desc >= p->max || !p->slots[desc].used)
        return NULL;
    return &p->slots[desc];
}

std_rw_result rom_win_read(const rom_win_pool_t *p, int desc, char *buf,
                           uint32_t count, uint32_t *got, api_errno *err)
{
    rom_win_t *w = rom_win_get(p, desc);
    if (!w)
    {
        *got = 0;
        *err = API_EBADF;
        return STD_ERROR;
    }
    uint32_t avail = w->pos < w->len ? w->len - w->pos : 0;
    if (count > avail)
        count = avail;
    if (!count)
    {
        *got = 0;
        return STD_OK; /* the window's end, which is EOF and not an error */
    }
    std_rw_result r = p->fetch(w, w->base + w->pos, buf, count, got, err);
    if (r == STD_OK)
        w->pos += *got;
    return r;
}

int rom_win_lseek(const rom_win_pool_t *p, int desc, int8_t whence, int32_t off,
                  int32_t *pos, api_errno *err)
{
    rom_win_t *w = rom_win_get(p, desc);
    if (!w)
    {
        *err = API_EBADF;
        return -1;
    }
    int32_t from = whence == SEEK_SET   ? 0
                   : whence == SEEK_CUR ? (int32_t)w->pos
                   : whence == SEEK_END ? (int32_t)w->len
                                        : -1;
    if (from < 0 || from + off < 0)
    {
        *err = API_EINVAL;
        return -1;
    }
    int32_t np = from + off;
    /* Past the end is where the window ends, not an error: the asset simply
     * stops there, and the next read says so by returning nothing. */
    if ((uint32_t)np > w->len)
        np = (int32_t)w->len;
    w->pos = (uint32_t)np;
    *pos = np;
    return 0;
}
