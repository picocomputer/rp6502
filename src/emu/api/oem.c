/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/api/oem.h"
#include "emu/chips/rp6502.h"
#include "emu/sys/vga.h"
#include "term/font.h"

/* FatFs conversion (ffunicode.c). WCHAR/DWORD/WORD are uint16/32/16. */
extern uint16_t ff_uni2oem(uint32_t uni, uint16_t cp);
extern uint16_t ff_oem2uni(uint16_t oem, uint16_t cp);

#define OEM_DEFAULT_CODE_PAGE 437

static uint16_t oem_code_page = OEM_DEFAULT_CODE_PAGE;

/* The single-byte OEM code pages with BOTH a font glyph table (font.c) and a
 * FatFs ff_uni2oem conversion table — the ones that render AND convert. */
static bool oem_code_page_supported(uint16_t cp)
{
    static const uint16_t supported[] = {
        437, 720, 737, 771, 775, 850, 852, 855, 857,
        860, 861, 862, 863, 864, 865, 866, 869};
    for (unsigned i = 0; i < sizeof supported / sizeof *supported; i++)
        if (supported[i] == cp)
            return true;
    return false;
}

/* Cold-boot default only — not re-applied on machine reset, so an exec'd program
 * keeps the current page. vga_boot_console's font_init then loads this same 437
 * default into the font, keeping the font, code page, and attribute in sync. */
void oem_reset(void)
{
    oem_code_page = OEM_DEFAULT_CODE_PAGE;
}

uint16_t oem_get_code_page(void)
{
    return oem_code_page;
}

/* Best-effort, like the firmware's oem_set_code_page: an unsupported page is
 * left unchanged and reported false (the CLI rejects it; the syscall ignores). */
bool oem_set_code_page(uint16_t cp)
{
    if (!oem_code_page_supported(cp))
        return false;
    oem_code_page = cp;
    font_set_code_page(cp);
    return true;
}

/* The "run" accessors the vendored atr.c uses. The emulator has no config-vs-
 * running split (no CONFIG.SYS), so the effective page is the only page. */
uint16_t oem_get_code_page_run(void)
{
    return oem_code_page;
}

void oem_set_code_page_run(uint16_t cp)
{
    oem_set_code_page(cp); /* best-effort; unsupported pages left unchanged */
}

/* A deliberate local copy of the firmware's str_utf8_to_oem (str/str.c). That
 * file is the firmware's whole printf/format library; vendoring it for this one
 * helper isn't worth it, so the decode is duplicated here. Keep them in sync. */
unsigned char oem_utf8_to_oem(const char **p)
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
        cp = b0 & 0x1F, extra = 1;
    else if ((b0 & 0xF0) == 0xE0)
        cp = b0 & 0x0F, extra = 2;
    else if ((b0 & 0xF8) == 0xF0)
        cp = b0 & 0x07, extra = 3;
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
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return 0x7F;
    uint16_t oem = ff_uni2oem(cp, oem_code_page);
    return oem ? (unsigned char)oem : 0x7F;
}

/* The inverse of oem_utf8_to_oem: one OEM byte -> its UTF-8 encoding. The
 * single-byte pages we support stay in the BMP, so at most 3 bytes. */
int oem_to_utf8(unsigned char b, char *dst)
{
    uint32_t u = b;
    if (b >= 0x80)
    {
        u = ff_oem2uni(b, oem_code_page);
        if (!u)
            u = 0xFFFD; // unmappable -> U+FFFD
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

/* OEM bytes -> UTF-16 (active code page). ASCII passes through; a high byte maps via
 * ff_oem2uni (unmappable -> U+FFFD). At most wcount-1 units + a 0 terminator. */
int oem_to_wide(const char *s, uint16_t *w, int wcount)
{
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n < wcount - 1; p++)
    {
        uint16_t u = *p < 0x80 ? *p : ff_oem2uni(*p, oem_code_page);
        w[n++] = u ? u : 0xFFFD;
    }
    if (wcount > 0)
        w[n] = 0;
    return n;
}

/* UTF-16 -> OEM bytes (active code page), the inverse. A code unit above the single-
 * byte pages (or a lone surrogate) is unmappable and becomes 0x7F, like the firmware's
 * str_utf8_to_oem. At most dstsz-1 bytes + a NUL. */
size_t oem_from_wide(const uint16_t *w, char *dst, size_t dstsz)
{
    size_t n = 0;
    for (; *w && n + 1 < dstsz; w++)
    {
        unsigned char b = *w < 0x80 ? (unsigned char)*w
                                    : (unsigned char)ff_uni2oem(*w, oem_code_page);
        dst[n++] = b ? (char)b : 0x7F;
    }
    if (dstsz)
        dst[n] = 0;
    return n;
}
