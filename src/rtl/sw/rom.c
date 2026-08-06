/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The pocket port of the .rp6502 loader, from emu/emu/rom.c: the emulator's
 * streams the image from a host file, and this machine's is a buffer
 * already — the staging window the platform filled — the APF data slot, or the bridge model in
 * simulation. Same format, same rules: text records stream raw bytes into
 * the 6502's memory, a load never writes $FF00-$FFF9, the $FFFA-$FFFF
 * vectors land in the register cells with the SRAM keeping the shadow, and
 * both reset vector bytes must arrive or the image is rejected.
 *
 * Suspend, for whoever tries it: this file is the reason it is not a
 * flag flip. A ROM with bundled assets keeps reading its file for as
 * long as the program runs — a load is not a copy that finishes — and
 * what it reads through is a 32 KB window in the SDRAM. If the Pocket
 * cuts power to the store while asleep, the window and the fonts above
 * it come back holding whatever survived, and the program will not know.
 * Waking has to refill them, or prove the store persists.
 */

#include "font.h"
#include "mmio.h"
#include "msc.h"
#include "rom.h"

#include "ria/api/uni.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static uint32_t rom_pos, rom_end;

/* The image is a file on the host, not a copy in memory: slot 8 is bound
 * to it and this is the 32 KB of it currently in hand. A read outside
 * fetches the aligned window that holds it, so walking the image forward
 * costs one round trip per window and an asset seek costs one. */
static uint32_t rom_win_base, rom_win_len;

void rom_win_invalidate(void)
{
    rom_win_len = 0;
}

static bool rom_window(uint32_t at)
{
    if (rom_win_len && at >= rom_win_base && at < rom_win_base + rom_win_len)
        return true;
    if (at >= rom_end)
        return false;
    uint32_t base = at & ~(SLOT_WIN_SIZE - 1);
    uint32_t len = rom_end - base;
    if (len > SLOT_WIN_SIZE)
        len = SLOT_WIN_SIZE;
    if (!msc_slot_read(MSC_SLOT_ROM, base, len))
    {
        rom_win_len = 0;
        return false;
    }
    rom_win_base = base;
    rom_win_len = len;
    return true;
}

static uint8_t rom_byte(uint32_t at)
{
    if (!rom_window(at))
        return 0;
    return SLOT_WIN(MSC_SLOT_ROM)[at - rom_win_base];
}
/* Where the asset directory starts, or 0 for an image without one. */
static uint32_t rom_assets;

static uint32_t rom_crc32(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int k = 0; k < 8; k++)
        crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    return crc;
}

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

static bool parse_u32(const char **pp, uint32_t *out)
{
    const char *p = *pp;
    while (*p == ' ' || *p == '\t')
        p++;
    uint32_t v = 0;
    int n = 0;
    bool hex = false;
    if (*p == '$')
    {
        hex = true;
        p++;
    }
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        hex = true;
        p += 2;
    }
    if (hex)
        while (isxdigit((unsigned char)*p))
        {
            char c = *p++;
            int d = (c <= '9') ? c - '0' : (toupper((unsigned char)c) - 'A' + 10);
            v = v * 16 + (uint32_t)d;
            n++;
        }
    else
        while (isdigit((unsigned char)*p))
        {
            v = v * 10 + (uint32_t)(*p++ - '0');
            n++;
        }
    if (!n)
        return false;
    *pp = p;
    *out = v;
    return true;
}

static bool parse_end(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return *p == 0;
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
    rom_win_len = 0;
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
        if (!parse_u32(&p, &chunks_len) || !parse_u32(&p, &image_crc))
            return false;
        prog_end = rom_pos + chunks_len;
    }
    else
        rom_pos = after_magic;
    rom_assets = prog_end < rom_end ? prog_end : 0;

    bool reset_lo = false, reset_hi = false;
    while (rom_pos < prog_end)
    {
        n = rom_gets(line, sizeof(line));
        if (n < 0)
            break;
        if (n == 0 || line[0] == '#')
            continue;
        const char *p = line;
        uint32_t addr, reclen, crc;
        if (!parse_u32(&p, &addr) || !parse_u32(&p, &reclen) ||
            !parse_u32(&p, &crc) || !parse_end(p))
            return false;
        /* The emulator's record rule verbatim: RAM below 0x10000, XRAM
         * above, and a record never straddles the boundary. */
        if (addr > 0x1FFFF || reclen == 0 || reclen > 0x20000 - addr ||
            (addr < 0x10000 && reclen > 0x10000 - addr))
            return false;
        if (rom_end - rom_pos < reclen)
            return false;
        uint32_t c = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < reclen; i++)
        {
            uint32_t a = addr + i;
            uint8_t b = rom_byte(rom_pos++);
            c = rom_crc32(c, b);
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
        if (addr <= 0xFFFC && addr + reclen > 0xFFFC)
            reset_lo = true;
        if (addr <= 0xFFFD && addr + reclen > 0xFFFD)
            reset_hi = true;
    }

    return reset_lo && reset_hi;
}

/* ---- The ROM: drive: read-only windows onto assets in the staged image.
 *
 * emu/emu/rom.c does this against a host file and keeps an fd positioned
 * for each window. Here the image is a host file too — slot 8 is bound to
 * it — so a window is an offset and a length, and a read is a copy when
 * the bytes are in hand and a round trip when they are not.
 *
 * An asset is named in UTF-8 and a program's path is code page bytes, so
 * the comparison converts as it walks. The two agree for the ASCII names
 * every toolchain actually emits and would disagree above 0x7F, which is
 * why the comparison goes through the code page tables.
 * ---- */

/* One window per stdio descriptor, so the pool is never what runs out
 * first. STD_FD_MAX is private to std.c, so the number is repeated rather
 * than shared; if that pool grows, this follows it. */
#define ROM_OPEN_MAX 16

static struct
{
    bool used;
    uint32_t base, len, pos;
} rom_win_pool[ROM_OPEN_MAX];

static bool rom_path_is_rom(const char *path, const char **rest)
{
    if (rom_strncasecmp(path, "ROM:", 4) != 0)
        return false;
    *rest = path + 4;
    return true;
}

/* The header's name against the program's path, one converted to the
 * other as it goes. */
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

/* Walk the "#>len crc name" headers from the directory start, skipping each
 * body, until the name matches or the list runs out. No index, so a program
 * may carry any number of assets. */
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
        if (!parse_u32(&p, &alen) || !parse_u32(&p, &acrc))
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
    for (int d = 0; d < ROM_OPEN_MAX; d++)
        if (!rom_win_pool[d].used)
        {
            rom_win_pool[d].used = true;
            rom_win_pool[d].base = base;
            rom_win_pool[d].len = len;
            rom_win_pool[d].pos = 0;
            return d;
        }
    *err = API_EMFILE;
    return -1;
}

static int rom_win(int desc)
{
    if (desc < 0 || desc >= ROM_OPEN_MAX || !rom_win_pool[desc].used)
        return -1;
    return desc;
}

std_rw_result rom_std_close(int desc, api_errno *err)
{
    if (rom_win(desc) < 0)
    {
        *err = API_EBADF;
        return STD_ERROR;
    }
    rom_win_pool[desc].used = false;
    return STD_OK;
}

std_rw_result rom_std_read(int desc, char *buf, uint32_t count,
                           uint32_t *got, api_errno *err)
{
    if (rom_win(desc) < 0)
    {
        *got = 0;
        *err = API_EBADF;
        return STD_ERROR;
    }
    uint32_t pos = rom_win_pool[desc].pos, len = rom_win_pool[desc].len;
    uint32_t avail = pos < len ? len - pos : 0;
    uint32_t want = count < avail ? count : avail;
    uint32_t at = rom_win_pool[desc].base + pos;
    for (uint32_t i = 0; i < want; i++)
        buf[i] = (char)rom_byte(at + i);
    rom_win_pool[desc].pos = pos + want;
    *got = want; /* short or zero at the window's end, which is EOF */
    return STD_OK;
}

int rom_std_lseek(int desc, int8_t whence, int32_t off, int32_t *pos,
                  api_errno *err)
{
    if (rom_win(desc) < 0)
    {
        *err = API_EBADF;
        return -1;
    }
    int32_t from = whence == SEEK_SET   ? 0
                   : whence == SEEK_CUR ? (int32_t)rom_win_pool[desc].pos
                   : whence == SEEK_END ? (int32_t)rom_win_pool[desc].len
                                        : -1;
    if (from < 0 || from + off < 0)
    {
        *err = API_EINVAL;
        return -1;
    }
    int32_t np = from + off;
    if ((uint32_t)np > rom_win_pool[desc].len)
        np = (int32_t)rom_win_pool[desc].len;
    rom_win_pool[desc].pos = (uint32_t)np;
    *pos = np;
    return 0;
}
