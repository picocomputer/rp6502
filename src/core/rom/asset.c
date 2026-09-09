/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The ROM: drive: read-only windows onto the loaded .rp6502's named assets.
 * The loader hands over its descriptor and where the asset directory begins;
 * an open scans the file for the entry and reads it on demand, so no index and
 * no asset bytes are held in RAM.
 */

#include "osal/fs.h"
#include "core/api/proc.h"
#include "core/rom/rom.h"
#include "core/str/str.h"
#include "core/str/oem.h"
#include "core/str/unicode.h"
#include <ctype.h>
#include <stdio.h> /* SEEK_SET */
#include <string.h>
#include <strings.h>

#define ROM_OPEN_MAX 16

/* Every window reads through this one descriptor, the loader's, on the running
 * program's own .rp6502. Closing a window does not close it, because the next
 * asset the program opens needs it still there. */
static int rom_fd = -1;
static uint32_t rom_assets_start;
static uint32_t g_rom_generation;

void rom_asset_adopt(int fd, uint32_t assets_start)
{
    rom_fd = fd;
    rom_assets_start = assets_start;
    g_rom_generation++;
}

void rom_assets_reset(void)
{
    if (rom_fd >= 0)
    {
        api_errno ignored;
        fs_std_close(rom_fd, &ignored);
        rom_fd = -1;
    }
    rom_assets_start = 0;
}

uint32_t rom_generation(void) { return g_rom_generation; }

static uint32_t rom_image_len(void)
{
    int32_t end = 0;
    api_errno ignored;
    if (rom_fd < 0 || fs_std_lseek(rom_fd, SEEK_END, 0, &end, &ignored) != 0 || end < 0)
        return 0;
    return (uint32_t)end;
}

void asset_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_bool(c, rom_fd >= 0);
    sst_put_u32(c, rom_assets_start);
    sst_put_u32(c, rom_image_len());
    sst_put_u32(c, g_rom_generation);
}

bool asset_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    bool open = sst_get_bool(c);
    uint32_t start = sst_get_u32(c);
    uint32_t len = sst_get_u32(c);
    uint32_t gen = sst_get_u32(c);
    if (!sst_ok(c))
        return false;

    rom_assets_reset();
    g_rom_generation = gen;
    if (!open)
        return true;

    /* proc_running() names the image this drive was reading only because
     * PROC_DRIVER precedes ASSET_DRIVER in the roster and sst_load walks it
     * forward; the name is reopened the way the load first opened it. */
    const char *running = proc_running();
    if (!running || !running[0])
        return false;
    api_errno err;
    int fd = fs_rom_open(rom_alias_resolve(running), FS_RD, &err);
    if (fd < 0)
        return false;
    rom_fd = fd;
    if (rom_image_len() != len)
    {
        rom_assets_reset();
        return false;
    }
    int32_t landed;
    fs_std_lseek(rom_fd, SEEK_SET, 0, &landed, &err);
    rom_assets_start = start;
    return true;
}

int rom_asset_fd(void) { return rom_fd; }

uint32_t rom_asset_dir(void) { return rom_assets_start; }

/* Waits out an asynchronous file driver's STD_PENDING, as pump.c's pump_read
 * does. Only the directory scan and rom_read_asset may wait like this; a window
 * read hands STD_PENDING back instead, because the guest's own call is
 * re-dispatched on it. */
static std_rw_result asset_read(void *buf, uint32_t count, uint32_t *got,
                                api_errno *err)
{
    std_rw_result r;
    do
        r = fs_std_read(rom_fd, buf, count, got, err);
    while (r == STD_PENDING);
    return r;
}

static bool asset_seek(uint32_t pos)
{
    int32_t landed;
    api_errno ignored;
    return fs_std_lseek(rom_fd, SEEK_SET, (int32_t)pos, &landed, &ignored) == 0;
}

static long asset_gets(uint32_t *pos, char *line, size_t cap)
{
    if (!asset_seek(*pos))
        return -1;
    uint32_t got = 0;
    api_errno ignored;
    if (asset_read(line, (uint32_t)cap - 1, &got, &ignored) != STD_OK || got == 0)
    {
        line[0] = 0;
        return -1;
    }
    size_t i = 0;
    while (i < got && line[i] != '\n')
        i++;
    *pos += (uint32_t)(i < got ? i + 1 : got);
    if (i && line[i - 1] == '\r')
        i--;
    line[i] = 0;
    return (long)i;
}

/* An asset is named in UTF-8 in the file and a program names it in code page
 * bytes, and the two agree only up to 0x7F, so the comparison converts as it
 * walks. */
static bool asset_name_eq(const char *utf8, const char *oem)
{
    uint16_t page = oem_get_code_page_run();
    for (;;)
    {
        unsigned char a = unicode_from_utf8_next(&utf8, page);
        unsigned char b = (unsigned char)*oem++;
        if (toupper(a) != toupper(b))
            return false;
        if (!a)
            return true;
    }
}

/* Find the asset named `name`. Each entry in the directory is a "#>$len $crc
 * name" header followed by that many bytes, so the walk reads a header and
 * skips a body until the name matches or a line turns up that is not a header.
 * On success *base is the file offset of the asset's data and *len its
 * length. */
bool rom_asset_find(const char *name, uint32_t *base, uint32_t *len)
{
    if (!rom_assets_start || rom_fd < 0)
        return false;
    uint32_t pos = rom_assets_start;
    char line[512];
    while (asset_gets(&pos, line, sizeof line) > 0 &&
           line[0] == '#' && line[1] == '>')
    {
        const char *p = line + 2;
        uint32_t alen, acrc;
        if (!str_parse_uint32(&p, &alen) || !str_parse_uint32(&p, &acrc))
            break;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p && asset_name_eq(p, name))
        {
            *base = pos;
            *len = alen;
            return true;
        }
        pos += alen;
    }
    return false;
}

long rom_read_asset(const char *name, char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0)
        return -1;
    buf[0] = 0;
    uint32_t base, len;
    if (!rom_asset_find(name, &base, &len))
        return -1;
    if (!asset_seek(base))
        return -1;
    uint32_t want = (len < bufsz - 1) ? len : (uint32_t)bufsz - 1;
    uint32_t got = 0;
    api_errno ignored;
    if (asset_read(buf, want, &got, &ignored) != STD_OK)
        return -1;
    buf[got] = 0;
    return (long)got;
}

typedef struct
{
    bool used;
    uint32_t base, len, pos;
} window_t;

static window_t windows[ROM_OPEN_MAX];

/* Every window shares the one descriptor, so each fetch seeks to the offset it
 * wants rather than reading on from wherever the last window left off. */
static std_rw_result window_fetch(uint32_t at, char *buf, uint32_t count,
                                  uint32_t *got, api_errno *err)
{
    int32_t landed;
    api_errno ignored;
    fs_std_lseek(rom_fd, SEEK_SET, (int32_t)at, &landed, &ignored);
    return fs_std_read(rom_fd, buf, count, got, err);
}

static window_t *window_get(int desc)
{
    if (desc < 0 || desc >= ROM_OPEN_MAX || !windows[desc].used)
        return NULL;
    return &windows[desc];
}

static bool path_is_rom(const char *path, const char **rest)
{
    if (strncasecmp(path, "ROM:", 4) == 0)
    {
        *rest = path + 4;
        return true;
    }
    return false;
}

bool rom_std_handles(const char *path)
{
    const char *rest;
    return path_is_rom(path, &rest);
}

int rom_std_open(const char *path, uint8_t flags, api_errno *err)
{
    const char *rest;
    if (!path_is_rom(path, &rest))
    {
        *err = API_ENOENT;
        return -1;
    }
    if (flags & FS_WR)
    {
        *err = API_EACCES;
        return -1;
    }
    uint32_t base, len;
    if (!rom_asset_find(rest, &base, &len))
    {
        *err = API_ENOENT;
        return -1;
    }
    for (int i = 0; i < ROM_OPEN_MAX; i++)
        if (!windows[i].used)
        {
            windows[i] = (window_t){.used = true, .base = base, .len = len};
            return i;
        }
    *err = API_EMFILE;
    return -1;
}

/* A window carries no name, because the image it is a range inside is the one
 * the ASSET chunk reopens. The three numbers are all of it, so a reopen
 * allocates a window rather than looking anything up. */
bool rom_std_ident(int desc, sst_cursor_t *c)
{
    window_t *w = window_get(desc);
    if (!w)
        return false;
    sst_put_u32(c, w->base);
    sst_put_u32(c, w->len);
    sst_put_u32(c, w->pos);
    return sst_ok(c);
}

int rom_std_reopen(sst_cursor_t *c, api_errno *err)
{
    uint32_t base = sst_get_u32(c);
    uint32_t len = sst_get_u32(c);
    uint32_t pos = sst_get_u32(c);
    if (!sst_ok(c) || pos > len)
    {
        *err = API_EINVAL;
        return -1;
    }
    for (int i = 0; i < ROM_OPEN_MAX; i++)
        if (!windows[i].used)
        {
            windows[i] = (window_t){.used = true, .base = base, .len = len, .pos = pos};
            return i;
        }
    *err = API_EMFILE;
    return -1;
}

std_rw_result rom_std_close(int desc, api_errno *err)
{
    window_t *w = window_get(desc);
    if (!w)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    w->used = false; /* the descriptor is the loader's and outlives the window */
    return STD_OK;
}

std_rw_result rom_std_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err)
{
    window_t *w = window_get(desc);
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
        return STD_OK; /* the end of the window is EOF, not an error */
    }
    std_rw_result r = window_fetch(w->base + w->pos, buf, count, got, err);
    if (r == STD_OK)
        w->pos += *got;
    return r;
}

int rom_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos, api_errno *err)
{
    window_t *w = window_get(desc);
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
    /* A seek past the end of the asset lands at its end rather than failing,
     * and the next read says so by returning nothing. */
    if ((uint32_t)np > w->len)
        np = (int32_t)w->len;
    w->pos = (uint32_t)np;
    *pos = np;
    return 0;
}
