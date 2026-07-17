/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "api/api.h"
#include "api/oem.h"
#include "hid/kbd.h"
#include "sys/cfg.h"
#include "sys/vga.h"
#include <fatfs/ff.h>
#include <pico.h>
#include <string.h>

#if defined(DEBUG_RIA_API) || defined(DEBUG_RIA_API_OEM)
#include <stdio.h>
#define DBG(...) printf(__VA_ARGS__)
#else
static inline void DBG(const char *fmt, ...) { (void)fmt; }
#endif

static uint16_t oem_code_page_set;
static uint16_t oem_code_page_run;
static uint16_t oem_auto_cp;

// Resolve the code page to apply: the override if set, else the locale auto.
static uint16_t oem_resolve(void)
{
    return oem_code_page_set ? oem_code_page_set : oem_auto_cp;
}

static void oem_request_code_page(uint16_t cp)
{
    uint16_t old_code_page = oem_code_page_run;
    // cp >= 900 are DBCS; allow SBCS only
    if (cp < 900 && f_setcp(cp) == FR_OK)
        oem_code_page_run = cp;
    if (old_code_page != oem_code_page_run)
    {
        vga_set_code_page(oem_code_page_run);
        kbd_rebuild_code_page_cache();
    }
}

void __in_flash("oem_init") oem_init(void)
{
    // Nothing loaded from config (no CONFIG.SYS): default to auto.
    if (!oem_code_page_run)
    {
        oem_code_page_set = 0;
        oem_request_code_page(oem_resolve());
    }
}

void oem_stop(void)
{
    if (oem_code_page_run != oem_resolve())
        oem_request_code_page(oem_resolve());
}

void oem_set_code_page_run(uint16_t cp)
{
    oem_request_code_page(cp);
}

bool oem_set_code_page(uint16_t cp)
{
    if (cp == 0)
    {
        // Auto: track the locale's default code page.
        if (oem_code_page_set != 0)
        {
            oem_code_page_set = 0;
            oem_request_code_page(oem_resolve());
            cfg_save();
        }
        return true;
    }
    oem_request_code_page(cp);
    if (cp != oem_code_page_run)
        return false;
    if (oem_code_page_set != cp)
    {
        oem_code_page_set = cp;
        cfg_save();
    }
    return true;
}

uint16_t oem_get_code_page(void)
{
    return oem_code_page_set;
}

bool oem_is_auto(void)
{
    return oem_code_page_set == 0;
}

uint16_t oem_get_code_page_run(void)
{
    return oem_code_page_run;
}

void oem_locale_changed(uint16_t cp)
{
    oem_auto_cp = cp;
    if (oem_code_page_set == 0)
        oem_request_code_page(oem_resolve());
}

void oem_load_code_page(uint16_t cp)
{
    oem_code_page_set = cp; // 0 = auto; legacy non-zero = hard override
    oem_request_code_page(oem_resolve());
}

unsigned char oem_from_codepoint(uint32_t cp)
{
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return 0x7F;
    WCHAR w = ff_uni2oem(cp, oem_code_page_run);
    return w ? (unsigned char)w : 0x7F;
}

unsigned char oem_from_utf8_next(const char **p)
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
    return oem_from_codepoint(cp);
}

int oem_to_utf8_char(unsigned char b, char *dst)
{
    uint32_t u = b;
    if (b >= 0x80)
    {
        u = ff_oem2uni(b, oem_code_page_run);
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

// Truncation never splits a sequence: once one doesn't fit, writing stops
// but the needed length keeps counting.
size_t oem_to_utf8(const char *s, char *dst, size_t dstsz)
{
    size_t need = 0;
    size_t out = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    {
        char enc[3];
        size_t n = (size_t)oem_to_utf8_char(*p, enc);
        if (dstsz && out == need && need + n < dstsz)
        {
            memcpy(dst + out, enc, n);
            out += n;
        }
        need += n;
    }
    if (dstsz)
        dst[out] = 0;
    return need;
}

size_t oem_from_utf8(const char *u8, char *dst, size_t dstsz)
{
    size_t need = 0;
    const char *p = u8;
    unsigned char b;
    while ((b = oem_from_utf8_next(&p)))
    {
        if (dstsz && need + 1 < dstsz)
            dst[need] = (char)b;
        need++;
    }
    if (dstsz)
        dst[need < dstsz ? need : dstsz - 1] = 0;
    return need;
}

int oem_to_wide(const char *s, uint16_t *w, int wcount)
{
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n < wcount - 1; p++)
    {
        uint16_t u = *p < 0x80 ? *p : ff_oem2uni(*p, oem_code_page_run);
        w[n++] = u ? u : 0xFFFD;
    }
    if (wcount > 0)
        w[n] = 0;
    return n;
}

size_t oem_from_wide_n(const uint16_t *w, size_t wlen, char *dst, size_t dstsz)
{
    size_t n = 0;
    for (size_t i = 0; i < wlen && n + 1 < dstsz; i++)
    {
        unsigned char b = w[i] < 0x80 ? (unsigned char)w[i]
                                      : (unsigned char)ff_uni2oem(w[i], oem_code_page_run);
        dst[n++] = b ? (char)b : 0x7F;
    }
    if (dstsz)
        dst[n] = 0;
    return n;
}

size_t oem_from_wide(const uint16_t *w, char *dst, size_t dstsz)
{
    size_t len = 0;
    while (w[len])
        len++;
    return oem_from_wide_n(w, len, dst, dstsz);
}
