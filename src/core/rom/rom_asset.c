/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The ROM: drive: read-only windows onto the loaded .rp6502's named assets,
 * for every machine. The loader hands over its descriptor and where the
 * asset directory begins; an open scans the file for the entry and reads it
 * on demand -- no index, no bytes in RAM, any number of assets.
 */

#include "core/api/fs.h"
#include "core/rom/rom.h"
#include "core/rom/rom_rec.h"
#include "core/rom/rom_win.h"
#include "core/str/str.h"
#include "core/str/unicode.h"
#include "core/term/font.h"
#include <ctype.h>
#include <stdio.h> /* SEEK_SET */
#include <string.h>
#include <strings.h>

#define ROM_OPEN_MAX 16 /* concurrent ROM: window opens (cf. the std fd pool) */

/* The loader's descriptor on the running program's own .rp6502 and the file
 * offset where its asset directory begins (0 = no assets). Every window reads
 * through the one descriptor; a window closing does not close it, because the
 * next asset the program opens wants it still there. */
static int rom_fd = -1;
static uint32_t rom_assets_start;
static uint32_t g_rom_generation; /* bumped per adopt; ROM Help watches it */

void rom_asset_adopt(int fd, uint32_t assets_start)
{
    rom_fd = fd;
    rom_assets_start = assets_start;
    g_rom_generation++;
}

/* Forget the loaded ROM's assets when a new program loads (exec/boot). Open
 * windows are closed separately by the machine reset. */
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

/* Reads on the ROM descriptor spin out STD_PENDING; see rom.c's pump_read.
 * Windows hand PENDING through instead (rom_fetch below) -- the guest's
 * dispatcher re-issues those. This blocking form serves the directory scan
 * and the host-side asset reader. */
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

/* One directory line, as the pump reads its lines: block read, scan to the
 * newline, remember where the next line starts. */
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

/* An asset is named in the file's UTF-8 and a program's path is code page
 * bytes, so the comparison converts as it walks; the two disagree above
 * 0x7F. The program header's entry has no name and matches nothing. */
static bool asset_name_eq(const char *utf8, const char *oem)
{
    uint16_t page = font_get_code_page();
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

/* Scan the asset directory for `name` (the text after "ROM:"). On success
 * *base is the file offset of its data and *len its length. Walk the
 * "#>len crc name" headers from the directory start, skipping each body,
 * until the name matches or the list ends. */
static bool rom_find_asset(const char *name, uint32_t *base, uint32_t *len)
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
        pos += alen; /* skip the body */
    }
    return false;
}

/* Read a named asset into buf (NUL-terminated, truncated to bufsz-1). Returns
 * bytes read, or -1 if no ROM is loaded or the asset is absent. Host-side
 * reader for the debugger's ROM Help viewer; the guest reads via ROM:. */
long rom_read_asset(const char *name, char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0)
        return -1;
    buf[0] = 0;
    uint32_t base, len;
    if (!rom_find_asset(name, &base, &len))
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

/* ---- The ROM: file driver (read-only asset windows), for std.c's table ---- */

static rom_win_t windows[ROM_OPEN_MAX];

/* Every window shares the loader's one descriptor, so a fetch says where it
 * wants to read rather than reading on from wherever the last one left off.
 * The seek's own outcome goes to scratch: a clamp is not this read's failure,
 * and the read that follows reports for itself. */
static std_rw_result rom_fetch(rom_win_t *w, uint32_t at, char *buf,
                               uint32_t count, uint32_t *got, api_errno *err)
{
    int32_t landed;
    api_errno ignored;
    fs_std_lseek(w->fd, SEEK_SET, (int32_t)at, &landed, &ignored);
    return fs_std_read(w->fd, buf, count, got, err);
}

static const rom_win_pool_t rom_pool = {windows, ROM_OPEN_MAX, rom_fetch};

/* If path names the ROM drive, return true and the asset name after "ROM:". */
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
    if (flags & FS_WR) /* write requested on a read-only asset */
    {
        *err = API_EACCES;
        return -1;
    }
    uint32_t base, len;
    if (!rom_find_asset(rest, &base, &len))
    {
        *err = API_ENOENT;
        return -1;
    }
    return rom_win_alloc(&rom_pool, base, len, rom_fd, err);
}

std_rw_result rom_std_close(int desc, api_errno *err)
{
    rom_win_t *w = rom_win_get(&rom_pool, desc);
    if (!w)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    w->used = false; /* the descriptor is the loader's, and outlives the window */
    return STD_OK;
}

std_rw_result rom_std_read(int desc, char *buf, uint32_t count, uint32_t *got, api_errno *err)
{
    return rom_win_read(&rom_pool, desc, buf, count, got, err);
}

int rom_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos, api_errno *err)
{
    return rom_win_lseek(&rom_pool, desc, whence, off, pos, err);
}
