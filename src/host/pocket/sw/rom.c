/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The pocket port of the .rp6502 loader, from core/sys/rom.c. The
 * emulator streams from a host file; here the whole image is resident in
 * the staging store. Same format, same rules: a load never writes
 * $FF00-$FFF9, the $FFFA-$FFFF vectors land in the register cells with
 * the SRAM keeping the shadow, and both reset vector bytes must arrive
 * or the image is rejected.
 */

#include "font.h"
#include "mmio.h"
#include "rom.h"
#include "core/sys/rom_rec.h"
#include "core/sys/rom_win.h"

#include "core/api/uni.h"
#include "core/mem.h"
#include "core/str/str.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static uint32_t rom_pos, rom_end;

uint32_t rom_staged_len(void)
{
    return rom_end;
}

static uint8_t rom_byte(uint32_t at)
{
    return at < rom_end ? ROM_IMG[at] : 0;
}
/* Where the asset directory starts, or 0 for an image without one. */
static uint32_t rom_assets;

/* One text line, NUL-terminated, CR/LF stripped, capped; length or -1 at
 * end with nothing read. The position is left at the first byte after the
 * newline — the start of a record's raw data, or the next header. */
static long rom_gets(char *line, size_t cap)
{
    size_t i = 0;
    int c = -1;
    while (rom_pos < rom_end && (c = rom_byte(rom_pos++)) != '\n')
        if (i + 1 < cap)
            line[i++] = (char)c;
    if (c == -1 && i == 0)
    {
        line[0] = 0;
        return -1;
    }
    if (i && line[i - 1] == '\r')
        i--;
    line[i] = 0;
    return (long)i;
}


static int rom_strncasecmp(const char *a, const char *b, size_t n)
{
    while (n--)
    {
        int d = toupper((unsigned char)*a) - toupper((unsigned char)*b);
        if (d || !*a)
            return d;
        a++, b++;
    }
    return 0;
}

bool rom_load_staged(uint32_t len)
{
    char line[512];
    rom_pos = 0;
    rom_end = len;
    rom_assets = 0;

    if (rom_gets(line, sizeof(line)) < 0 ||
        rom_strncasecmp(line, "#!RP6502", 8) != 0)
        return false;

    /* Optional "#>$chunks_len $crc" header bounds the program records;
     * named assets follow. Classic format runs records to the end. */
    uint32_t after_magic = rom_pos;
    uint32_t prog_end = rom_end;
    long n = rom_gets(line, sizeof(line));
    if (n >= 2 && line[0] == '#' && line[1] == '>')
    {
        const char *p = line + 2;
        uint32_t chunks_len, image_crc;
        if (!str_parse_uint32(&p, &chunks_len) || !str_parse_uint32(&p, &image_crc))
            return false;
        prog_end = rom_pos + chunks_len;
    }
    else
        rom_pos = after_magic;
    rom_assets = prog_end < rom_end ? prog_end : 0;

    rom_rec_vectors_t vectors = {0};
    while (rom_pos < prog_end)
    {
        n = rom_gets(line, sizeof(line));
        if (n < 0)
            break;
        rom_rec_t rec;
        rom_rec_result r = rom_rec_parse(line, 0, &rec);
        if (r == ROM_REC_SKIP)
            continue;
        if (r != ROM_REC_OK)
            return false;
        const uint32_t addr = rec.addr, reclen = rec.len, crc = rec.crc;
        if (rom_end - rom_pos < reclen)
            return false;
        uint32_t c = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < reclen; i++)
        {
            uint32_t a = addr + i;
            uint8_t b = rom_byte(rom_pos++);
            c = mem_crc32(c, &b, 1);
            if (a > 0xFFFF)
                XRAM_WIN[a - 0x10000] = b;
            /* A load never writes the RIA window's low page; the vectors
             * land in the cells, the SRAM keeps the shadow. */
            else if (a < 0xFF00 || a >= 0xFFFA)
                SRAM[a] = b;
            if (a >= 0xFFFA && a <= 0xFFFF)
                REGS_WIN[a & 0x1F] = b;
        }
        if ((c ^ 0xFFFFFFFFu) != crc)
            return false;
        rom_rec_note(&vectors, &rec);
    }

    return rom_rec_complete(&vectors);
}

/* The ROM: drive: read-only windows onto assets in the staged image. An
 * asset is named in UTF-8 and a program's path is code page bytes, so
 * the comparison converts as it walks; the two disagree above 0x7F. */

/* One window per stdio descriptor, so the pool is never what runs out
 * first. STD_FD_MAX is private to std.c; if that pool grows, this
 * follows it. */
#define ROM_OPEN_MAX 16

static rom_win_t rom_slots[ROM_OPEN_MAX];

/* The whole image is already in the staging store, a byte at a time through
 * a window that cannot fetch anything wider. */
static std_rw_result rom_fetch(rom_win_t *w, uint32_t at, char *buf,
                               uint32_t count, uint32_t *got, api_errno *err)
{
    (void)w, (void)err;
    for (uint32_t i = 0; i < count; i++)
        buf[i] = (char)rom_byte(at + i);
    *got = count;
    return STD_OK;
}

static const rom_win_pool_t rom_pool = {rom_slots, ROM_OPEN_MAX, rom_fetch};

static bool rom_path_is_rom(const char *path, const char **rest)
{
    if (rom_strncasecmp(path, "ROM:", 4) != 0)
        return false;
    *rest = path + 4;
    return true;
}

static bool rom_name_eq(const char *utf8, const char *oem)
{
    uint16_t page = font_get_code_page();
    for (;;)
    {
        unsigned char a = uni_from_utf8_next(&utf8, page);
        unsigned char b = (unsigned char)*oem++;
        if (toupper(a) != toupper(b))
            return false;
        if (!a)
            return true;
    }
}

/* No index, so a program may carry any number of assets: walk the
 * "#>len crc name" headers, skipping each body. */
static bool rom_find_asset(const char *name, uint32_t *base, uint32_t *len)
{
    if (!rom_assets)
        return false;
    char line[512];
    rom_pos = rom_assets;
    while (rom_pos < rom_end)
    {
        long n = rom_gets(line, sizeof line);
        if (n < 2 || line[0] != '#' || line[1] != '>')
            return false;
        const char *p = line + 2;
        uint32_t alen, acrc;
        if (!str_parse_uint32(&p, &alen) || !str_parse_uint32(&p, &acrc))
            return false;
        while (*p == ' ' || *p == '\t')
            p++;
        uint32_t data = rom_pos;
        if (rom_name_eq(p, name))
        {
            *base = data;
            *len = alen;
            return true;
        }
        if (data + alen > rom_end)
            return false; /* truncated: no more assets */
        rom_pos = data + alen;
    }
    return false;
}

bool rom_std_handles(const char *path)
{
    const char *rest;
    return rom_path_is_rom(path, &rest);
}

int rom_std_open(const char *path, uint8_t flags, api_errno *err)
{
    const char *rest;
    if (!rom_path_is_rom(path, &rest))
    {
        *err = API_ENOENT;
        return -1;
    }
    if (flags & 0x02) /* an asset is read-only */
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
    return rom_win_alloc(&rom_pool, base, len, -1, err);
}

std_rw_result rom_std_close(int desc, api_errno *err)
{
    rom_win_t *w = rom_win_get(&rom_pool, desc);
    if (!w)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    w->used = false;
    return STD_OK;
}

std_rw_result rom_std_read(int desc, char *buf, uint32_t count,
                           uint32_t *got, api_errno *err)
{
    return rom_win_read(&rom_pool, desc, buf, count, got, err);
}

int rom_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos,
                  api_errno *err)
{
    return rom_win_lseek(&rom_pool, desc, whence, off, pos, err);
}
