/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * OEM code page conversion, ours instead of FatFs's ffunicode.c.
 *
 * The vendored file is 1.36 MB and almost all of it is the double-byte
 * pages FF_NO_DBCS excludes. What survives the preprocessor is about
 * five kilobytes of table and three short functions, wrapped in enough
 * FF_CODE_PAGE conditionals that the functions cannot be read without
 * first deciding which build you are in. The tables now come out of
 * src/core/gen/oem_table_gen.py — same numbers, lifted from the
 * same file — and the logic is here, once, with no build to choose.
 *
 * There is a second reason. The Pocket has no room to link five
 * kilobytes into a 48 KB tightly coupled memory, so its tables ride in
 * the staging store beside the fonts and are read a word at a time
 * through a window that cannot fetch anything wider than a byte. That
 * only works if every table access goes through one function, which is
 * uni_word, and the platform supplies it.
 *
 * Conversion is therefore slower here than an array index, and on the
 * Pocket much slower. Nothing on a hot path converts: a code page
 * lookup happens when a filename is compared or a string is displayed,
 * never per character of a stream.
 */

/* FatFs where there is one: it declares these three itself, in types it
 * picks per platform, and a definition here has to be the same types. Not
 * every tree that compiles this file has the header — the simulation's
 * bench does not — so the fallback below is what its C99 branch would
 * have given us. */
#ifdef __has_include
#if __has_include(<fatfs/ff.h>)
#include <fatfs/ff.h>
#endif
#endif
#include "core/api/uni.h"

#ifndef FF_DEFINED
typedef uint16_t WCHAR;
typedef uint16_t WORD;
typedef uint32_t DWORD;
#endif

/* The image's header, as oem_table_gen.py lays it out. */
#define UNI_MAGIC 0x4F43u
#define UNI_PAGE_WORDS 128

static uint32_t uni_pages;
static uint32_t uni_cp_at;
static uint32_t uni_page_at;
static uint32_t uni_cvt1_at;
static uint32_t uni_cvt2_at;

bool uni_init(void)
{
    uint32_t c1 = uni_word(2);
    uni_pages = uni_word(1);
    uni_cp_at = 4;
    uni_page_at = uni_cp_at + uni_pages;
    uni_cvt1_at = uni_page_at + uni_pages * UNI_PAGE_WORDS;
    uni_cvt2_at = uni_cvt1_at + c1;
    return uni_word(0) == UNI_MAGIC;
}

/* A platform that links the tables in never fails, so it is spared
 * having to say so at boot; the first lookup reads the header. */
static void uni_ready(void)
{
    if (!uni_page_at)
        uni_init();
}

/* The image carries the page numbers in a run of its own, so a lookup
 * is a short scan rather than a table of pointers that would have to be
 * relocated for a machine reading them out of a file. */
static int uni_page(uint16_t cp)
{
    for (uint32_t i = 0; i < uni_pages; i++)
        if (uni_word(uni_cp_at + i) == cp)
            return (int)i;
    return -1;
}

/* Whether the tables carry a page at all. The filesystem below may keep a
 * page of its own, but it cannot spell a character these tables cannot. */
bool uni_has_page(uint16_t cp)
{
    uni_ready();
    return uni_page(cp) >= 0;
}

WCHAR ff_oem2uni(WCHAR oem, WORD cp)
{
    if (oem < 0x80)
        return oem; /* ASCII passes through every page */
    uni_ready();
    int p = uni_page(cp);
    if (p < 0)
        return 0;
    return uni_word(uni_page_at + (uint32_t)p * UNI_PAGE_WORDS
                    + (oem - 0x80u));
}

WCHAR ff_uni2oem(DWORD uni, WORD cp)
{
    if (uni < 0x80)
        return (uint16_t)uni;
    if (uni >= 0x10000)
        return 0; /* half a surrogate pair is not an OEM character */
    uni_ready();
    int p = uni_page(cp);
    if (p < 0)
        return 0;
    uint32_t at = uni_page_at + (uint32_t)p * UNI_PAGE_WORDS;
    for (uint32_t i = 0; i < UNI_PAGE_WORDS; i++)
        if (uni_word(at + i) == uni)
            return (uint16_t)(i + 0x80);
    return 0;
}

/* The up-case tables are runs, each headed by a base code point and a
 * word holding a command in its top byte and a length in its low one.
 * Command zero is followed by that many replacements; the rest are
 * arithmetic on the code point itself, which is how the whole of
 * Unicode's simple case mapping fits in a few hundred words. */
DWORD ff_wtoupper(DWORD uni)
{
    if (uni >= 0x10000)
        return uni; /* nothing outside the BMP has a simple up-case */
    uni_ready();
    uint16_t uc = (uint16_t)uni;
    uint32_t p = uc < 0x1000 ? uni_cvt1_at : uni_cvt2_at;
    for (;;)
    {
        uint16_t base = uni_word(p++);
        if (base == 0 || uc < base)
            break;
        uint16_t nc = uni_word(p++);
        uint16_t cmd = nc >> 8;
        nc &= 0xFF;
        if (uc < base + nc)
        {
            switch (cmd)
            {
            case 0:
                uc = uni_word(p + (uc - base));
                break;
            case 1:
                uc -= (uc - base) & 1; /* the pairs are lower, upper */
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

/* --- The UTF-8 codec. It lived in oem.c, which also owns the current
 * code page, the locale and telling the keyboard and the screen when it
 * changes — and reaches for four headers to do it, none of which exist
 * on a machine with no config store and its own video. The conversion
 * itself never needed any of them: only a page number and the two
 * lookups above. oem.c keeps its own names and passes the page it is
 * holding, so nothing that called it has changed. --- */

unsigned char uni_from_codepoint(uint32_t cp, uint16_t page)
{
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return 0x7F;
    uint16_t w = ff_uni2oem(cp, page);
    return w ? (unsigned char)w : 0x7F;
}

unsigned char uni_from_utf8_next(const char **p, uint16_t page)
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
    // Reject overlong forms and beyond-Unicode: untrusted input (host file
    // names) must not decode to an ASCII it doesn't contain ('/', '.').
    static const uint32_t min_cp[] = {0, 0x80, 0x800, 0x10000};
    if (cp < min_cp[extra] || cp > 0x10FFFF)
        return 0x7F;
    return uni_from_codepoint(cp, page);
}

int uni_to_utf8_char(unsigned char b, uint16_t page, char *dst)
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
