/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * OEM code page conversion, ours instead of FatFs's ffunicode.c. The tables
 * come out of src/core/str/unicode.py, which lifts them from that same
 * vendored file.
 */

/* FatFs where there is one: a definition here has to use the same types ff.h
 * picks, and not every tree that compiles this file has that header. */
#ifdef __has_include
#if __has_include(<fatfs/ff.h>)
#include <fatfs/ff.h>
#endif
#endif
#include "core/str/unicode.h"
#include "machine.h"

#include "unicode_oem.h"

#ifndef FF_DEFINED
typedef uint16_t WCHAR;
typedef uint16_t WORD;
typedef uint32_t DWORD;
#endif

static int HOST_IN_RAM_FUNC unicode_page(uint16_t cp)
{
    for (int i = 0; i < UNICODE_OEM_PAGES; i++)
        if (unicode_oem_cp[i] == cp)
            return i;
    return -1;
}

bool unicode_has_page(uint16_t cp)
{
    return unicode_page(cp) >= 0;
}

/* The tables start at 0x80 because ASCII is the same in every page. */
WCHAR HOST_IN_RAM_FUNC ff_oem2uni(WCHAR oem, WORD cp)
{
    if (oem < 0x80)
        return oem;
    int p = unicode_page(cp);
    if (p < 0)
        return 0;
    return unicode_oem_page[p][oem - 0x80u];
}

WCHAR HOST_IN_RAM_FUNC ff_uni2oem(DWORD uni, WORD cp)
{
    if (uni < 0x80)
        return (uint16_t)uni;
    if (uni >= 0x10000)
        return 0; /* the tables cover the BMP only */
    int p = unicode_page(cp);
    if (p < 0)
        return 0;
    for (uint32_t i = 0; i < 128; i++)
        if (unicode_oem_page[p][i] == uni)
            return (uint16_t)(i + 0x80);
    return 0;
}

/* The up-case tables are runs, each headed by a base code point and a word
 * holding a command in its top byte and a length in its low one. Command zero
 * is followed by that many replacement code points; every other command is
 * arithmetic on the code point itself, which is how the simple case mapping
 * fits in a few hundred words. */
DWORD HOST_IN_RAM_FUNC ff_wtoupper(DWORD uni)
{
    if (uni >= 0x10000)
        return uni; /* the tables cover the BMP only */
    uint16_t uc = (uint16_t)uni;
    const uint16_t *p = uc < 0x1000 ? unicode_oem_cvt1 : unicode_oem_cvt2;
    for (;;)
    {
        uint16_t base = *p++;
        if (base == 0 || uc < base)
            break;
        uint16_t nc = *p++;
        uint16_t cmd = nc >> 8;
        nc &= 0xFF;
        if (uc < base + nc)
        {
            switch (cmd)
            {
            case 0:
                uc = p[uc - base];
                break;
            case 1:
                uc -= (uc - base) & 1; /* the run is pairs of upper, lower */
                break;
            case 2:
                uc -= 16;
                break;
            case 3:
                uc -= 32;
                break;
            case 4:
                uc -= 48;
                break;
            case 5:
                uc -= 26;
                break;
            case 6:
                uc += 8;
                break;
            case 7:
                uc -= 80;
                break;
            default:
                uc -= 0x1C60;
                break;
            }
            return uc;
        }
        if (cmd == 0)
            p += nc;
    }
    return uc;
}

unsigned char unicode_from_codepoint(uint32_t cp, uint16_t page)
{
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return 0x7F;
    uint16_t w = ff_uni2oem(cp, page);
    return w ? (unsigned char)w : 0x7F;
}

unsigned char unicode_from_utf8_next(const char **p, uint16_t page)
{
    const unsigned char *s = (const unsigned char *)*p;
    unsigned char b0 = *s;
    if (!b0)
        return 0;
    if (b0 < 0x80)
    {
        *p = (const char *)(s + 1);
        return b0;
    }
    uint32_t cp;
    int extra;
    if ((b0 & 0xE0) == 0xC0)
    {
        cp = b0 & 0x1F;
        extra = 1;
    }
    else if ((b0 & 0xF0) == 0xE0)
    {
        cp = b0 & 0x0F;
        extra = 2;
    }
    else if ((b0 & 0xF8) == 0xF0)
    {
        cp = b0 & 0x07;
        extra = 3;
    }
    else
    {
        *p = (const char *)(s + 1);
        return 0x7F;
    }
    for (int i = 1; i <= extra; i++)
    {
        unsigned char bi = s[i];
        if ((bi & 0xC0) != 0x80)
        {
            *p = (const char *)(s + i);
            return 0x7F;
        }
        cp = (cp << 6) | (bi & 0x3F);
    }
    *p = (const char *)(s + extra + 1);
    // Overlong forms and code points beyond Unicode are rejected because this
    // decodes untrusted host filenames, which must not produce an ASCII byte
    // they do not contain, such as '/' or '.'.
    static const uint32_t min_cp[] = {0, 0x80, 0x800, 0x10000};
    if (cp < min_cp[extra] || cp > 0x10FFFF)
        return 0x7F;
    return unicode_from_codepoint(cp, page);
}

int unicode_to_utf8_char(unsigned char b, uint16_t page, char *dst)
{
    uint32_t u = b;
    if (b >= 0x80)
    {
        u = ff_oem2uni(b, page);
        if (!u)
            u = 0xFFFD;
    }
    if (u < 0x80)
    {
        dst[0] = (char)u;
        return 1;
    }
    if (u < 0x800)
    {
        dst[0] = (char)(0xC0 | (u >> 6));
        dst[1] = (char)(0x80 | (u & 0x3F));
        return 2;
    }
    dst[0] = (char)(0xE0 | (u >> 12));
    dst[1] = (char)(0x80 | ((u >> 6) & 0x3F));
    dst[2] = (char)(0x80 | (u & 0x3F));
    return 3;
}
