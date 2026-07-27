/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The pocket port of the .rp6502 loader, from emu/emu/rom.c: the emulator's
 * is buffer-based already, and this machine's buffer is the staging window
 * the platform filled — the APF data slot, or the bridge model in
 * simulation. Same format, same rules: text records stream raw bytes into
 * the 6502's memory, a load never writes $FF00-$FFF9, the $FFFA-$FFFF
 * vectors land in the register cells with the SRAM keeping the shadow, and
 * both reset vector bytes must arrive or the image is rejected.
 *
 */

#include "mmio.h"
#include "rom.h"

#include <ctype.h>

static uint32_t rom_pos, rom_end;

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
    while (rom_pos < rom_end && (c = STAGE[rom_pos++]) != '\n')
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
            uint8_t b = STAGE[rom_pos++];
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
