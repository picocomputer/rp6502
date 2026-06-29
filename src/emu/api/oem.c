/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * OEM code page (the emulator stand-in for ria/api/oem.c). On hardware oem.c
 * resolves an IBM/DOS code page from CONFIG.SYS or the host locale and drives
 * the VGA font, FatFs, and the keyboard. The emulator models only the effective
 * code page — no config file, no locale-auto (per design) — defaulting to 437,
 * and drives the terminal glyph table (font_set_code_page). clk.c reads it to
 * map strftime output to OEM glyphs and ff_uni2oem uses it for filenames.
 */

#include "emu/api/oem.h"
#include "emu/sys/ria.h"
#include "emu/sys/sys.h"
#include "emu/sys/vga.h"
#include "term/font.h"

/* FatFs Unicode->OEM mapping (ffunicode.c). WCHAR/DWORD/WORD are uint16/32/16. */
extern uint16_t ff_uni2oem(uint32_t uni, uint16_t cp);

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

/* Cold-boot default, called from emu_init (NOT ria_reset, so an exec'd program
 * keeps the current page). vga_boot_console's font_init then loads this same 437
 * default into the font, keeping the font, code page, and attribute in sync. */
void oem_reset(void)
{
    oem_code_page = OEM_DEFAULT_CODE_PAGE;
}

uint16_t oem_get_code_page(void)
{
    return oem_code_page;
}

/* Best-effort, like the firmware's oem_set_code_page_run: an unsupported page is
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
